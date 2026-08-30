// SPDX-License-Identifier: GPL-2.0
/*
 * DSA driver for the Realtek RTL8371C ("RTL8366UB") 2.5G switch as fitted to
 * the GL.iNet GL-MT5000 (Brume 3).
 *
 * Copyright (C) 2024 Jianhui Zhao <jianhui.zhao@gl-inet.com>
 *
 * Based on GL.iNet's DSA driver from OpenWrt PR #24237 (GLiNet-Tech/openwrt
 * commit d174ae5, force-pushed 2026-08-27, kernel 6.18). That version is
 * bench-validated - LAN1<->LAN2 and VLAN trunking both confirmed working on
 * real hardware - so it is the reference, and the changes below are layered
 * on top of it rather than the other way round:
 *
 *   1. PHY power up/down goes through the OCP accessor. GL reaches OCP
 *      register 0xa610 with rtk_port_phyReg_set(), the Clause-22 accessor,
 *      which rejects any reg > RTL8371C_PHY_REGNOMAX (31) with
 *      RT_ERR_PHY_REG_ID - so both its power-up and power-down writes are
 *      silently dropped. This uses the same read-modify-write on bit 11 that
 *      dal_rtl8371c_port_phyEnableAll_set() performs.
 *
 *   2. Spanning-tree state is written to MSTI 0, the instance every VLAN
 *      entry actually references (fid_msti = 0). GL writes MSTI 1, which no
 *      VLAN references, making .port_stp_state_set a hardware no-op.
 *      NOTE: this is the highest-risk change here - it takes a call that
 *      currently does nothing and makes it program real forwarding state.
 *      Revert this one first if ports come up blocked.
 *
 *   3. rtk_vlan_init() enables per-port ingress VLAN filtering on every valid
 *      port. DSA expects a switch that is transparent to VLAN tags until a
 *      bridge asks otherwise, so setup() clears it (including on the CPU
 *      trunk port, which DSA never toggles) and .port_vlan_filtering turns it
 *      back on per port. Without this a vlan_filtering=0 bridge silently
 *      drops any tagged frame whose VID the bridge never added.
 *
 *   4. .port_vlan_add / .port_vlan_del fail closed: a failed read of the
 *      current entry never falls back to a zeroed one, which would erase
 *      every other member port of that VID.
 *
 *   5. .port_fdb_add / .port_fdb_del plus assisted_learning_on_cpu_port: the
 *      rtl8_4 tagger sets LEARN_DIS on every CPU-injected frame, so without
 *      this the switch never learns the router's own MAC and floods replies.
 *
 *   6. .port_fast_age, .port_pre_bridge_flags / .port_bridge_flags (hardware
 *      learning toggle) and .port_change_mtu / .port_max_mtu, which must
 *      account for the 8-byte rtl8_4 tag on the CPU port.
 *
 *   7. .shutdown, and a small debugfs corner for bring-up.
 */

#include <linux/etherdevice.h>
#include <linux/if_bridge.h>
#include <linux/if_vlan.h>
#include <linux/debugfs.h>
#include <linux/mii.h>
#include <linux/of_net.h>
#include <linux/bitops.h>
#include <linux/phylink.h>
#include <net/dsa.h>
#include <net/switchdev.h>
#include <linux/gpio/consumer.h>

#include "rtk_switch.h"
#include "rtk_error.h"
#include "port.h"
#include "vlan.h"
#include "cpu.h"
#include "l2.h"
#include "rtl8366ub_dsa.h"
#include "dal/smi.h"

/* Length of the Realtek rtl8_4 CPU tag inserted on the CPU/extension port
 * (see net/dsa/tag_rtl8_4.c, RTL8_4_TAG_LEN).
 */
#define RTL8366UB_CPU_TAG_LEN	8

/* Hardware per-port max frame length ceiling: RTL8371C_MAX_PACKET_LENGTH
 * (0x3FEF) from the SDK DAL (dal/rtl8371c/dal_rtl8371c_port.h). The DAL
 * rejects any rtk_port_maxPacketLength_set() above this value.
 */
#define RTL8366UB_MAX_PKT_LEN	0x3FEF

/* OCP register holding the per-PHY power-down bit on the 2.5G-capable UTP
 * ports (bit 11), per dal_rtl8371c_port_phyEnableAll_set().
 */
#define RTL8366UB_PHY_OCP_PWR	0xa610
#define RTL8366UB_PHY_OCP_PDOWN	0x0800

static int rtl8366ub_sdk_errno(rtksw_api_ret_t ret)
{
    switch (ret) {
    case RT_ERR_OK:
        return 0;
    case RT_ERR_INPUT:
    case RT_ERR_UNIT_ID:
    case RT_ERR_PORT_ID:
    case RT_ERR_PORT_MASK:
    case RT_ERR_NULL_POINTER:
    case RT_ERR_OUT_OF_RANGE:
        return -EINVAL;
    case RT_ERR_BUSYWAIT_TIMEOUT:
        return -ETIMEDOUT;
    case RT_ERR_NOT_INIT:
    case RT_ERR_CHIP_NOT_FOUND:
        return -ENODEV;
    case RT_ERR_CHIP_NOT_SUPPORTED:
    case RT_ERR_DRIVER_NOT_FOUND:
        return -EOPNOTSUPP;
    default:
        return -EIO;
    }
}

static int rtl8366ub_sdk_read(struct rtl8366ub_priv *priv, u32 reg, u32 *val)
{
    return rtl8366ub_sdk_errno(reg_smi_read(0, reg, val));
}

static int rtl8366ub_find_cpu_port(struct dsa_switch *ds)
{
    /* Find the connected cpu port. Valid port are 3 or 8 */

    if (dsa_is_cpu_port(ds, EXT_PORT0))
        return EXT_PORT0;

    if (dsa_is_cpu_port(ds, EXT_PORT1))
        return EXT_PORT1;

    return -EINVAL;
}

static enum dsa_tag_protocol rtl8366ub_sw_get_tag_protocol(struct dsa_switch *ds, int port,
                                                           enum dsa_tag_protocol mp)
{
    struct rtl8366ub_priv *priv = ds->priv;

    if (port != EXT_PORT0 && port != EXT_PORT1) {
        dev_warn(priv->dev, "port not matched with tagging CPU port\n");
        return DSA_TAG_PROTO_NONE;
    } else {
        return DSA_TAG_PROTO_RTL8_4;
    }
}

/*
 * Power a single user-port PHY up or down.
 *
 * GL's driver writes 0x2058 / 0x2858 to 0xa610 through rtk_port_phyReg_set(),
 * but dal_rtl8371c_port_phyReg_set() range-checks reg against
 * RTL8371C_PHY_REGNOMAX (31) and returns RT_ERR_PHY_REG_ID, so those writes
 * never reach the chip. The DAL's own phyEnableAll_set() reaches 0xa610 via
 * the OCP accessor and flips bit 11 read-modify-write; mirror that, and fall
 * back to the plain C22 BMCR power-down bit on non-2.5G ports exactly as the
 * DAL does.
 */
static int rtl8366ub_phy_power(int port, bool on)
{
    rtksw_port_phy_data_t data;
    rtksw_api_ret_t ret;

    if (rtksw_switch_isUtp2p5gPort(0, port) == RT_ERR_OK) {
        ret = rtk_port_phyOCPReg_get(port, RTL8366UB_PHY_OCP_PWR, &data);
        if (ret != RT_ERR_OK)
            return rtl8366ub_sdk_errno(ret);

        if (on)
            data &= ~(u32)RTL8366UB_PHY_OCP_PDOWN;
        else
            data |= RTL8366UB_PHY_OCP_PDOWN;

        ret = rtk_port_phyOCPReg_set(port, RTL8366UB_PHY_OCP_PWR, data);
    } else {
        ret = rtk_port_phyReg_get(port, MII_BMCR, &data);
        if (ret != RT_ERR_OK)
            return rtl8366ub_sdk_errno(ret);

        if (on)
            data &= ~(u32)BMCR_PDOWN;
        else
            data |= BMCR_PDOWN;

        ret = rtk_port_phyReg_set(port, MII_BMCR, data);
    }

    return rtl8366ub_sdk_errno(ret);
}

static int rtl8366ub_cpu_mac_config(struct dsa_switch *ds, rtksw_port_t port,
                                    phy_interface_t phy_mode)
{
    struct rtl8366ub_priv *priv = ds->priv;
    rtksw_port_mac_ability_t mac_cfg = { 0 };
    rtksw_mode_ext_t mode_ext;
    int ret;

    mode_ext = RTKSW_MODE_EXT_HSGMII;
    mac_cfg.forcemode = PORT_MAC_FORCE;
    mac_cfg.speed = RTKSW_PORT_SPEED_2500M;
    mac_cfg.duplex = RTKSW_PORT_FULL_DUPLEX;
    mac_cfg.link = RTKSW_PORT_LINKUP;
    mac_cfg.nway = RTKSW_DISABLED;
    mac_cfg.txpause = RTKSW_ENABLED;
    mac_cfg.rxpause = RTKSW_ENABLED;

    switch (phy_mode) {
        case PHY_INTERFACE_MODE_2500BASEX:
            mac_cfg.speed = RTKSW_PORT_SPEED_2500M;
            mode_ext = RTKSW_MODE_EXT_HSGMII;
            break;
        case PHY_INTERFACE_MODE_10GKR:
        case PHY_INTERFACE_MODE_USXGMII:
            mac_cfg.speed = RTKSW_PORT_SPEED_10G;
            mode_ext = RTKSW_MODE_EXT_USXGMII;
            break;
        default:
            dev_err(priv->dev, "phy mode %s not supported for CPU port\n",
                    phy_modes(phy_mode));
            return -EOPNOTSUPP;
    }

    ret = rtk_port_macForceLinkExt_set(port, mode_ext, &mac_cfg);
    if (ret != RT_ERR_OK) {
        dev_err(priv->dev, "failed to configure CPU port %u: %d\n",
                port, ret);
        return -EIO;
    }

    return 0;
}

static int rtl8366ub_sw_setup(struct dsa_switch *ds)
{
    struct rtl8366ub_priv *priv = ds->priv;
    rtksw_portmask_t portmask;
    rtksw_portmask_t cpu_portmask;
    phy_interface_t phy_mode;
    int ret, i;
    u32 val;

    /* Reset whole chip through gpio pin or memory-mapped registers for
     * different type of hardware
     */
    gpiod_set_value_cansleep(priv->reset, 0);
    usleep_range(100000, 150000);
    gpiod_set_value_cansleep(priv->reset, 1);
    usleep_range(1000000, 1500000);

    /* Detect device */
    ret = rtl8366ub_sdk_read(priv, 0x4, &val);
    if (ret) {
        dev_err(priv->dev, "can't get chip ID (%d)\n", ret);
        return ret;
    }

    switch (val) {
        case 0x8366:
            dev_info(priv->dev, "found an RTL8366UB switch\n");
            break;
        default:
            dev_err(priv->dev, "found an Unknown Realtek switch (id=0x%04x)\n",
                    val);
            return -ENODEV;
    }

    priv->cpu_port = rtl8366ub_find_cpu_port(ds);
    if (priv->cpu_port < 0) {
        dev_err(priv->dev, "No cpu port configured in both cpu port3 and port8");
        return -EINVAL;
    }

    ret = of_get_phy_mode(dsa_to_port(ds, priv->cpu_port)->dn, &phy_mode);
    if (ret) {
        dev_err(priv->dev, "can't get phy-mode for CPU port: %d\n", ret);
        return ret;
    }

    ret = rtk_switch_init();
    if (ret != RT_ERR_OK)
        return dev_err_probe(priv->dev, -EIO,
                             "failed to initialize switch: %d\n", ret);

    ret = rtk_vlan_init();
    if (ret != RT_ERR_OK)
        return dev_err_probe(priv->dev, -EIO,
                             "failed to initialize VLANs: %d\n", ret);

    RTKSW_PORTMASK_CLEAR(portmask);
    for (i = 0; i < RTL8366UB_NUM_PORTS; i++) {
        RTKSW_PORTMASK_CLEAR(cpu_portmask);
        RTKSW_PORTMASK_PORT_SET(cpu_portmask, priv->cpu_port);
        rtk_port_isolation_set(i, &cpu_portmask);

        RTKSW_PORTMASK_PORT_SET(portmask, i);

        /*
         * rtk_vlan_init() turns on per-port ingress VLAN filtering, which
         * drops tagged frames whose VID has no 4k-table membership. DSA
         * expects a non-filtering switch by default (transparent to VLAN
         * tags); .port_vlan_filtering re-enables it per port when a bridge
         * requests vlan_filtering=1.
         */
        rtk_vlan_portIgrFilterEnable_set(i, RTKSW_DISABLED);
    }
    rtk_port_isolation_set(EXT_PORT0, &portmask);
    rtk_port_isolation_set(priv->cpu_port, &portmask);

    /* rtk_vlan_init() turns ingress VLAN filtering on for ALL valid ports,
     * including the CPU/extension port. The loop above only clears it for the
     * user ports, and DSA only ever toggles .port_vlan_filtering on user
     * ports - so the CPU port would stay filtered forever. The CPU port is a
     * trunk that must accept every bridge VLAN (and CPU-injected frames);
     * leave its ingress filter off. Egress is still governed by the per-VID
     * member set programmed in .port_vlan_add.
     */
    rtk_vlan_portIgrFilterEnable_set(priv->cpu_port, RTKSW_DISABLED);

    ret = rtl8366ub_cpu_mac_config(ds, priv->cpu_port, phy_mode);
    if (ret)
        return ret;

    /* Also writes CPU_TAG_AWARE_CTRL = BIT(phys cpu port), i.e. makes the CPU
     * port parse an inbound 0x8899 tag when one is present. Frames arriving
     * from the CPU without that Length/Type - such as anything the MediaTek
     * PPE emits on the conduit - simply take the normal FDB path.
     */
    ret = rtk_cpu_tagPort_set(priv->cpu_port, CPU_INSERT_TO_ALL);
    if (ret != RT_ERR_OK)
        return dev_err_probe(priv->dev, -EIO,
                             "failed to configure CPU tag port: %d\n", ret);

    ret = rtk_cpu_enable_set(RTKSW_ENABLED);
    if (ret != RT_ERR_OK)
        return dev_err_probe(priv->dev, -EIO,
                             "failed to enable CPU tagging: %d\n", ret);

    ret = rtk_port_phyEnableAll_set(RTKSW_ENABLED);
    if (ret != RT_ERR_OK)
        return dev_err_probe(priv->dev, -EIO,
                             "failed to enable PHYs: %d\n", ret);

    return 0;
}

static int rtl8366ub_sw_phy_read(struct dsa_switch *ds, int port, int regnum)
{
    rtksw_port_phy_data_t val;
    int ret;

    ret = rtk_port_phyReg_get(port, regnum, &val);
    if (ret != RT_ERR_OK)
        return -EIO;

    return val;
}

static int rtl8366ub_sw_phy_write(struct dsa_switch *ds, int port, int regnum, u16 val)
{
    if (rtk_port_phyReg_set(port, regnum, val) != RT_ERR_OK)
        return -EIO;

    return 0;
}

static int rtl8366ub_sw_port_enable(struct dsa_switch *ds, int port,
                                    struct phy_device *phy)
{
    struct rtl8366ub_priv *priv = ds->priv;
    int ret;

    if (!dsa_is_user_port(ds, port))
        return 0;

    mutex_lock(&priv->reg_mutex);
    priv->ports[port].enable = true;
    ret = rtl8366ub_phy_power(port, true);
    mutex_unlock(&priv->reg_mutex);

    return ret;
}

static void rtl8366ub_sw_port_disable(struct dsa_switch *ds, int port)
{
    struct rtl8366ub_priv *priv = ds->priv;

    if (!dsa_is_user_port(ds, port))
        return;

    mutex_lock(&priv->reg_mutex);
    priv->ports[port].enable = false;
    rtl8366ub_phy_power(port, false);
    mutex_unlock(&priv->reg_mutex);
}

static void rtl8366ub_sw_stp_state_set(struct dsa_switch *ds, int port, u8 state)
{
    u32 stp_state;

    if (dsa_is_unused_port(ds, port))
        return;

    switch (state) {
        case BR_STATE_DISABLED:
            stp_state = RTKSW_STP_STATE_DISABLED;
            break;
        case BR_STATE_BLOCKING:
        case BR_STATE_LISTENING:
            stp_state = RTKSW_STP_STATE_BLOCKING;
            break;
        case BR_STATE_LEARNING:
            stp_state = RTKSW_STP_STATE_LEARNING;
            break;
        case BR_STATE_FORWARDING:
        default:
            stp_state = RTKSW_STP_STATE_FORWARDING;
            break;
    }

    /* All VLAN entries carry fid_msti=0, so the spanning-tree state must be
     * written to MSTI 0 - the instance that actually governs their traffic.
     * GL writes MSTI 1, an instance no VLAN references, making this a
     * hardware no-op.
     */
    rtk_stp_mstpState_set(0, port, stp_state);
}

static int rtl8366ub_sw_port_bridge_join(struct dsa_switch *ds, int port,
                                         struct dsa_bridge bridge,
                                         bool *tx_fwd_offload,
                                         struct netlink_ext_ack *extack)
{
    struct rtl8366ub_priv *priv = ds->priv;
    rtksw_portmask_t portmask_tmp;
    rtksw_portmask_t portmask;
    int i;

    /* The isolation update is a read-modify-write spanning several SDK calls
     * (rtk_port_isolation_get + _set). Hold reg_mutex so it cannot interleave
     * with another port's join/leave or a vlan_add/del RMW. rtnl_lock
     * serialises these switchdev ops today, so this is consistency rather
     * than an active race, but the SDK-level lock only makes each individual
     * get/set atomic, not the get..set pair.
     */
    mutex_lock(&priv->reg_mutex);

    rtk_port_isolation_get(port, &portmask);

    for (i = 0; i < RTL8366UB_NUM_PORTS; i++) {
        if (i == port)
            continue;

        if (!dsa_port_offloads_bridge(dsa_to_port(ds, i), &bridge))
            continue;

        /* Join this port to each other port on the bridge */
        rtk_port_isolation_get(i, &portmask_tmp);
        RTKSW_PORTMASK_PORT_SET(portmask_tmp, port);
        rtk_port_isolation_set(i, &portmask_tmp);

        RTKSW_PORTMASK_PORT_SET(portmask, i);
    }

    /* Join each other port on the bridge to this port */
    rtk_port_isolation_set(port, &portmask);

    mutex_unlock(&priv->reg_mutex);

    return 0;
}

static void rtl8366ub_sw_port_bridge_leave(struct dsa_switch *ds, int port,
                                           struct dsa_bridge bridge)
{
    struct rtl8366ub_priv *priv = ds->priv;
    rtksw_portmask_t portmask_tmp;
    rtksw_portmask_t portmask;
    int i;

    /* See port_bridge_join: hold reg_mutex over the isolation get..set RMW. */
    mutex_lock(&priv->reg_mutex);

    rtk_port_isolation_get(port, &portmask);

    for (i = 0; i < RTL8366UB_NUM_PORTS; i++) {
        if (i == port)
            continue;

        if (!dsa_port_offloads_bridge(dsa_to_port(ds, i), &bridge))
            continue;

        /* Remove this port from any other port on the bridge */
        rtk_port_isolation_get(i, &portmask_tmp);
        RTKSW_PORTMASK_PORT_CLEAR(portmask_tmp, port);
        rtk_port_isolation_set(i, &portmask_tmp);

        RTKSW_PORTMASK_PORT_CLEAR(portmask, i);
    }

    rtk_port_isolation_set(port, &portmask);

    mutex_unlock(&priv->reg_mutex);
}

static int rtl8366ub_sw_port_vlan_filtering(struct dsa_switch *ds, int port,
                                            bool vlan_filtering,
                                            struct netlink_ext_ack *extack)
{
    struct rtl8366ub_priv *priv = ds->priv;

    /* Per-port ingress VLAN filtering: only enforce membership when the
     * bridge actually asks for vlan_filtering=1. setup() leaves it off so a
     * plain (non-filtering) bridge is transparent to tags.
     */
    mutex_lock(&priv->reg_mutex);
    rtk_vlan_portIgrFilterEnable_set(port,
        vlan_filtering ? RTKSW_ENABLED : RTKSW_DISABLED);
    mutex_unlock(&priv->reg_mutex);

    return 0;
}

static int rtl8366ub_sw_port_vlan_add(struct dsa_switch *ds, int port,
                                      const struct switchdev_obj_port_vlan *vlan,
                                      struct netlink_ext_ack *extack)
{
    struct rtl8366ub_priv *priv = ds->priv;
    bool untagged = vlan->flags & BRIDGE_VLAN_INFO_UNTAGGED;
    bool pvid = vlan->flags & BRIDGE_VLAN_INFO_PVID;
    rtksw_vlan_cfg_t vlan_entry;
    u16 vid = vlan->vid;

    mutex_lock(&priv->reg_mutex);

    /* Read-modify-write: keep the existing members of this VLAN so a bridge
     * spanning several ports does not lose members as each port is added one
     * at a time. A read failure must NOT fall back to a zeroed entry - writing
     * that back would erase every other member of the VID.
     */
    if (rtk_vlan_get(vid, &vlan_entry) != RT_ERR_OK) {
        mutex_unlock(&priv->reg_mutex);
        NL_SET_ERR_MSG_MOD(extack, "failed to read current VLAN entry");
        return -EIO;
    }

    RTK_PORTMASK_PORT_SET(vlan_entry.mbr, port);
    if (untagged)
        RTK_PORTMASK_PORT_SET(vlan_entry.untag, port);
    else
        RTK_PORTMASK_PORT_CLEAR(vlan_entry.untag, port);
    vlan_entry.ivl_en = 1;

    rtk_vlan_set(vid, &vlan_entry);

    if (pvid) {
        rtk_vlan_portPvid_set(port, vid, 0);
        if (port < RTL8366UB_NUM_PORTS)
            priv->ports[port].pvid = vid;
    }

    mutex_unlock(&priv->reg_mutex);

    return 0;
}

static int rtl8366ub_sw_port_vlan_del(struct dsa_switch *ds, int port,
                                      const struct switchdev_obj_port_vlan *vlan)
{
    struct rtl8366ub_priv *priv = ds->priv;
    rtksw_vlan_cfg_t vlan_entry;
    u16 vid = vlan->vid;
    u16 pvid;

    mutex_lock(&priv->reg_mutex);

    /* A read failure must not fall back to a zeroed entry: writing that back
     * would wipe the other member ports of this VID. Skip the membership
     * write and only fix up PVID bookkeeping below.
     */
    if (rtk_vlan_get(vid, &vlan_entry) == RT_ERR_OK) {
        /* Only drop this port from the VLAN, keep any other members intact. */
        RTK_PORTMASK_PORT_CLEAR(vlan_entry.mbr, port);
        RTK_PORTMASK_PORT_CLEAR(vlan_entry.untag, port);
        rtk_vlan_set(vid, &vlan_entry);
    }

    if (port < RTL8366UB_NUM_PORTS) {
        pvid = priv->ports[port].pvid;
        if (pvid == vid) {
            rtk_vlan_portPvid_set(port, 1, 0);
            priv->ports[port].pvid = 1;
        }
    }

    mutex_unlock(&priv->reg_mutex);

    return 0;
}

static void rtl8366ub_sw_port_fast_age(struct dsa_switch *ds, int port)
{
    struct rtl8366ub_priv *priv = ds->priv;
    rtksw_l2_flushCfg_t cfg;

    /* Flush only the dynamically-learned unicast entries on this port. The
     * config is zero-initialised, so flushByVid/flushByFid/flushByMac stay
     * disabled and the SDK takes the flush-by-port path; flushStaticAddr=0
     * keeps static (bridge-offloaded) FDB entries intact. 'port' is the DSA
     * index == SDK logical port and is L2P-mapped inside the DAL.
     */
    memset(&cfg, 0, sizeof(cfg));
    cfg.flushByPort = RTKSW_ENABLED;
    cfg.port = port;
    cfg.flushStaticAddr = RTKSW_DISABLED;

    mutex_lock(&priv->reg_mutex);
    rtk_l2_ucastAddr_flush(&cfg);
    mutex_unlock(&priv->reg_mutex);
}

static int rtl8366ub_sw_port_pre_bridge_flags(struct dsa_switch *ds, int port,
                                              struct switchdev_brport_flags flags,
                                              struct netlink_ext_ack *extack)
{
    /* Only per-port MAC learning can be toggled in hardware. */
    if (flags.mask & ~(BR_LEARNING))
        return -EINVAL;

    return 0;
}

static int rtl8366ub_sw_port_bridge_flags(struct dsa_switch *ds, int port,
                                          struct switchdev_brport_flags flags,
                                          struct netlink_ext_ack *extack)
{
    struct rtl8366ub_priv *priv = ds->priv;
    rtksw_api_ret_t ret = RT_ERR_OK;

    if (flags.mask & BR_LEARNING) {
        /* A per-port auto-learn limit of 0 disables SA learning; the chip's
         * max LUT count re-enables it (mirrors rtl8365mb's learn-limit
         * approach). maxLutAddrNumber_get() returns 0 before init, but this
         * op only runs after setup() has completed rtk_switch_init().
         */
        mutex_lock(&priv->reg_mutex);
        ret = rtk_l2_limitLearningCnt_set(port,
                (flags.val & BR_LEARNING) ?
                    rtksw_switch_maxLutAddrNumber_get(0) : 0);
        mutex_unlock(&priv->reg_mutex);
    }

    return (ret == RT_ERR_OK) ? 0 : -EIO;
}

static int rtl8366ub_sw_port_fdb_add(struct dsa_switch *ds, int port,
                                     const unsigned char *addr, u16 vid,
                                     struct dsa_db db)
{
    struct rtl8366ub_priv *priv = ds->priv;
    rtksw_l2_ucastAddr_t l2;
    rtksw_mac_t mac;
    rtksw_api_ret_t ret;

    memcpy(mac.octet, addr, ETH_ALEN);

    memset(&l2, 0, sizeof(l2));
    l2.port = port;
    l2.is_static = 1;
    if (vid) {
        l2.ivl = 1;             /* IVL: entry keyed on (MAC, cvid) */
        l2.cvid = vid;
    } else {
        l2.ivl = 0;             /* SVL: entry keyed on (MAC, fid 0) */
        l2.fid = 0;
    }

    mutex_lock(&priv->reg_mutex);
    ret = rtk_l2_addr_add(&mac, &l2);
    mutex_unlock(&priv->reg_mutex);

    return (ret == RT_ERR_OK) ? 0 : -EIO;
}

static int rtl8366ub_sw_port_fdb_del(struct dsa_switch *ds, int port,
                                     const unsigned char *addr, u16 vid,
                                     struct dsa_db db)
{
    struct rtl8366ub_priv *priv = ds->priv;
    rtksw_l2_ucastAddr_t l2;
    rtksw_mac_t mac;
    rtksw_api_ret_t ret;

    memcpy(mac.octet, addr, ETH_ALEN);

    memset(&l2, 0, sizeof(l2));
    if (vid) {
        l2.ivl = 1;
        l2.cvid = vid;
    } else {
        l2.ivl = 0;
        l2.fid = 0;
    }

    mutex_lock(&priv->reg_mutex);
    ret = rtk_l2_addr_del(&mac, &l2);
    mutex_unlock(&priv->reg_mutex);

    /* A missing entry is not an error for an idempotent delete. */
    if (ret == RT_ERR_OK || ret == RT_ERR_L2_ENTRY_NOTFOUND)
        return 0;

    return -EIO;
}

static int rtl8366ub_sw_port_change_mtu(struct dsa_switch *ds, int port,
                                        int new_mtu)
{
    struct rtl8366ub_priv *priv = ds->priv;
    rtksw_api_ret_t ret;
    u32 frame_size;

    /* MTU is the L3 payload; the switch polices the whole L2 frame, so add
     * Ethernet + one VLAN tag + FCS overhead (1500 -> 1522).
     */
    frame_size = new_mtu + VLAN_ETH_HLEN + ETH_FCS_LEN;

    /* Frames crossing the CPU/extension port additionally carry the 8-byte
     * rtl8_4 DSA tag, so that port must accept the extra bytes. DSA drives
     * this op for each user port (dp->index) and, via the MTU notifier, for
     * the CPU port with cpu_mtu == largest user MTU (tag NOT included), so we
     * add it here.
     */
    if (dsa_is_cpu_port(ds, port))
        frame_size += RTL8366UB_CPU_TAG_LEN;

    mutex_lock(&priv->reg_mutex);
    ret = rtk_port_maxPacketLength_set(port, frame_size);
    mutex_unlock(&priv->reg_mutex);

    return (ret == RT_ERR_OK) ? 0 : -EIO;
}

static int rtl8366ub_sw_port_max_mtu(struct dsa_switch *ds, int port)
{
    /* Largest L3 MTU that still fits the hardware frame ceiling once the L2
     * overhead and the rtl8_4 CPU tag are accounted for. DSA clamps this
     * against the conduit's own max_mtu.
     */
    return RTL8366UB_MAX_PKT_LEN - VLAN_ETH_HLEN - ETH_FCS_LEN -
           RTL8366UB_CPU_TAG_LEN;
}

static void rtl8366ub_sw_phylink_get_caps(struct dsa_switch *ds, int port,
                                          struct phylink_config *config)
{
    if (port < RTL8366UB_NUM_PORTS) {
        __set_bit(PHY_INTERFACE_MODE_INTERNAL,
                  config->supported_interfaces);
        __set_bit(PHY_INTERFACE_MODE_GMII,
                  config->supported_interfaces);
        config->mac_capabilities = MAC_ASYM_PAUSE | MAC_SYM_PAUSE |
                                   MAC_10 | MAC_100 | MAC_1000FD |
                                   MAC_2500FD;
        return;
    }

    __set_bit(PHY_INTERFACE_MODE_2500BASEX,
              config->supported_interfaces);
    __set_bit(PHY_INTERFACE_MODE_USXGMII,
              config->supported_interfaces);
    config->mac_capabilities = MAC_ASYM_PAUSE | MAC_SYM_PAUSE |
                               MAC_1000FD | MAC_2500FD | MAC_10000FD;
}

static void rtl8366ub_sw_phylink_mac_config(struct phylink_config *config,
        unsigned int mode, const struct phylink_link_state *state)
{
}

static void rtl8366ub_sw_phylink_mac_link_down(struct phylink_config *config,
        unsigned int mode, phy_interface_t interface)
{
    struct dsa_port *dp = dsa_phylink_to_port(config);
    int port = dp->index;
    rtksw_port_mac_ability_t mac_cfg = { 0 };

    if (port >= RTL8366UB_NUM_PORTS)
        return;

    mac_cfg.link = RTKSW_PORT_LINKDOWN;
    rtk_port_macForceLink_set(port, &mac_cfg);
}

static void rtl8366ub_sw_phylink_mac_link_up(struct phylink_config *config,
        struct phy_device *phydev, unsigned int mode,
        phy_interface_t interface, int speed, int duplex,
        bool tx_pause, bool rx_pause)
{
    struct dsa_port *dp = dsa_phylink_to_port(config);
    int port = dp->index;
    rtksw_port_mac_ability_t mac_cfg = { 0 };

    if (port >= RTL8366UB_NUM_PORTS)
        return;

    /* @speed is trustworthy here: rtl8366ub_phy.c implements .read_status via
     * rtk_port_phyStatus_get(), so 2.5G resolution is reported correctly
     * rather than being capped at 1000 by generic C22 phylib.
     */
    switch (speed) {
        case SPEED_10:
            mac_cfg.speed = RTKSW_PORT_SPEED_10M;
            break;
        case SPEED_100:
            mac_cfg.speed = RTKSW_PORT_SPEED_100M;
            break;
        case SPEED_1000:
            mac_cfg.speed = RTKSW_PORT_SPEED_1000M;
            break;
        case SPEED_2500:
            mac_cfg.speed = RTKSW_PORT_SPEED_2500M;
            break;
        default:
            mac_cfg.speed = RTKSW_PORT_SPEED_2500M;
    }
    mac_cfg.forcemode = PORT_MAC_NORMAL;
    mac_cfg.duplex = duplex;
    mac_cfg.link = RTKSW_PORT_LINKUP;
    mac_cfg.nway = RTKSW_DISABLED;
    mac_cfg.txpause = tx_pause;
    mac_cfg.rxpause = rx_pause;

    rtk_port_macForceLink_set(port, &mac_cfg);
}

static const struct phylink_mac_ops rtl8366ub_phylink_mac_ops = {
    .mac_config = rtl8366ub_sw_phylink_mac_config,
    .mac_link_down = rtl8366ub_sw_phylink_mac_link_down,
    .mac_link_up = rtl8366ub_sw_phylink_mac_link_up,
};

/*
 * Concurrency and MDIO-bus locking - verified correct, documented so neither
 * is "fixed" into a regression:
 *
 *   (a) The SDK's global rtksw_api_mutex that serialises every rtk_*() call is
 *       DEFINE_MUTEX(rtksw_api_mutex) in rtk_switch.c. It is statically
 *       initialised - do NOT add a mutex_init() for it and do NOT try to
 *       "cover" it with priv->reg_mutex. In the kernel build RTK_X86_CLE is
 *       undefined, so RTKSW_API_LOCK expands to mutex_lock(&rtksw_api_mutex)
 *       (not pthread): every rtk_* entry point holds it around register
 *       access, so all chip access is globally serialised. Lock ordering is
 *       consistent (priv->reg_mutex always outer, rtksw_api_mutex always
 *       inner) so there is no AB-BA deadlock.
 *
 *   (b) dal/smi.c holds __mii_bus->mdio_lock across the whole indirect
 *       transaction and issues it via the __mdiobus_* (unlocked) accessors -
 *       exactly the in-tree realtek-mdio.c pattern. That is what keeps switch
 *       indirect access atomic against the WAN c45 PHY at mdio addr 15 on the
 *       shared bus. Do NOT "simplify" smi.c to the self-locking
 *       mdiobus_read/write: that would drop the lock between the address-latch
 *       write and the data read and let the WAN-PHY poll corrupt it.
 */

/*
 * CPU-tag / port-ID mapping - verified safe, documented so it is not "fixed"
 * into a blackout:
 *
 * tag_rtl8_4 (DSA_TAG_PROTO_RTL8_4) carries the switch port in the DSA
 * port-index number space: on RX it reads TX = source port and calls
 * dsa_conduit_find_user(dev, 0, port) (matches dp->index); on TX it writes
 * RX mask = BIT(dp->index). This mirrors the in-tree rtl8365mb driver, where
 * dp->index IS the chip port number.
 *
 * On the RTL8371C the SDK logical<->physical map is the IDENTITY for the UTP
 * ports: logical 0..3 -> physical 0..3. The two MT5000 user ports are DTS
 * port@0 (lan1) and port@1 (lan2), i.e. dp->index 0/1 == chip physical 0/1.
 * So the chip emits TX=0/1 for frames from lan1/lan2 and honours RX mask
 * BIT(0)/BIT(1) to egress on lan1/lan2 - exactly what the tagger assumes.
 *
 * The CPU port is DTS port@17 = EXT_PORT1 (logical 17), which the SDK maps to
 * physical 7. This logical!=physical divergence NEVER enters the tag TX/RX
 * fields for user traffic (the CPU port is never a tag source or an xmit
 * destination), so it is harmless. Do NOT "correct" the CPU port to reg=7:
 * rtl8366ub_find_cpu_port() only accepts EXT_PORT0(16)/EXT_PORT1(17), and the
 * SDK performs the 17->7 translation internally.
 */
static const struct dsa_switch_ops rtl8366ub_switch_ops = {
    .get_tag_protocol = rtl8366ub_sw_get_tag_protocol,
    .setup = rtl8366ub_sw_setup,
    .phy_read = rtl8366ub_sw_phy_read,
    .phy_write = rtl8366ub_sw_phy_write,
    .port_enable = rtl8366ub_sw_port_enable,
    .port_disable = rtl8366ub_sw_port_disable,
    .port_stp_state_set = rtl8366ub_sw_stp_state_set,
    .port_bridge_join = rtl8366ub_sw_port_bridge_join,
    .port_bridge_leave = rtl8366ub_sw_port_bridge_leave,
    .port_vlan_filtering = rtl8366ub_sw_port_vlan_filtering,
    .port_vlan_add = rtl8366ub_sw_port_vlan_add,
    .port_vlan_del = rtl8366ub_sw_port_vlan_del,
    .port_fast_age = rtl8366ub_sw_port_fast_age,
    .port_pre_bridge_flags = rtl8366ub_sw_port_pre_bridge_flags,
    .port_bridge_flags = rtl8366ub_sw_port_bridge_flags,
    .port_fdb_add = rtl8366ub_sw_port_fdb_add,
    .port_fdb_del = rtl8366ub_sw_port_fdb_del,
    .port_change_mtu = rtl8366ub_sw_port_change_mtu,
    .port_max_mtu = rtl8366ub_sw_port_max_mtu,
    .phylink_get_caps = rtl8366ub_sw_phylink_get_caps,
};

static int port_isolation_show(struct seq_file *s, void *v)
{
    rtksw_portmask_t mask;
    int i;

    for (i = 0; i < RTL8366UB_NUM_PORTS; i++) {
        rtk_port_isolation_get(i, &mask);
        seq_printf(s, "port%d: 0x%x\n", i, mask.bits[0]);
    }

    return 0;
}
DEFINE_SHOW_ATTRIBUTE(port_isolation);

static ssize_t phy_reg_read(struct file *file,
                            const char __user *user_buf,
                            size_t count, loff_t *ppos)
{
    rtksw_port_phy_data_t val = 0;
    char buf[256] = "";
    unsigned int phy, reg;

    if (copy_from_user(buf, user_buf, min(count, sizeof(buf) - 1)))
        return -EFAULT;

    if (sscanf(buf, "%u %x", &phy, &reg) != 2)
        return -EINVAL;

    if (rtk_port_phyReg_get(phy, reg, &val) != RT_ERR_OK)
        return -EIO;

    pr_info("phy: %u, reg: 0x%x = 0x%x\n", phy, reg, val);

    return count;
}

static const struct file_operations phy_reg_fops = {
    .open = simple_open,
    .write = phy_reg_read,
};

static int rtl8366ub_mdio_probe(struct mdio_device *mdiodev)
{
    struct rtl8366ub_priv *priv;
    int ret;

    priv = devm_kzalloc(&mdiodev->dev, sizeof(*priv), GFP_KERNEL);
    if (!priv)
        return -ENOMEM;

    priv->ds = devm_kzalloc(&mdiodev->dev, sizeof(*priv->ds), GFP_KERNEL);
    if (!priv->ds)
        return -ENOMEM;

    dev_info(&mdiodev->dev, "RTL8366UB DSA driver, version %s, mdio addr %d\n",
             DRIVER_VERSION, mdiodev->addr);

    priv->reset = devm_gpiod_get_optional(&mdiodev->dev, "reset",
                                          GPIOD_OUT_LOW);
    if (IS_ERR(priv->reset)) {
        dev_err(&mdiodev->dev, "Couldn't get our reset line\n");
        return PTR_ERR(priv->reset);
    }

    priv->bus = mdiodev->bus;
    priv->dev = &mdiodev->dev;
    priv->mdio_addr = mdiodev->addr;
    mutex_init(&priv->reg_mutex);
    /*
     * NOTE: the SDK's global rtksw_api_mutex that serialises every rtk_*()
     * call is a DEFINE_MUTEX() in rtk_switch.c (statically initialised) - do
     * NOT add a mutex_init() for it.
     */
    dev_set_drvdata(&mdiodev->dev, priv);

    priv->ds->dev = &mdiodev->dev;
    priv->ds->num_ports = EXT_PORT1 + 1;
    priv->ds->priv = priv;
    priv->ds->ops = &rtl8366ub_switch_ops;
    priv->ds->phylink_mac_ops = &rtl8366ub_phylink_mac_ops;

    /* The rtl8_4 tagger sets LEARN_DIS on every CPU-injected frame
     * (tag_rtl8_4.c), so the switch never learns the router/host MAC by
     * itself and would flood all replies. Let DSA software-learn conduit
     * source MACs and install them as static entries via .port_fdb_add on
     * the CPU port.
     */
    priv->ds->assisted_learning_on_cpu_port = true;

    rtk_set_mdc_mdio(priv->bus, priv->mdio_addr);

    ret = dsa_register_switch(priv->ds);
    if (ret) {
        if (ret == -EPROBE_DEFER)
            dev_err(priv->dev, "unable to register switch, deferred\n");
        else
            dev_err(priv->dev, "unable to register switch ret = %d\n", ret);
        return ret;
    }

    priv->dbgfs = debugfs_create_dir("rtl8366ub", NULL);
    debugfs_create_file("port_isolation", S_IRUSR, priv->dbgfs, NULL,
                        &port_isolation_fops);
    debugfs_create_file("phy_reg", S_IWUSR, priv->dbgfs, NULL, &phy_reg_fops);

    return 0;
}

static void rtl8366ub_mdio_remove(struct mdio_device *mdiodev)
{
    struct rtl8366ub_priv *priv = dev_get_drvdata(&mdiodev->dev);

    if (!priv)
        return;

    debugfs_remove_recursive(priv->dbgfs);
    dsa_unregister_switch(priv->ds);
}

static void rtl8366ub_mdio_shutdown(struct mdio_device *mdiodev)
{
    struct rtl8366ub_priv *priv = dev_get_drvdata(&mdiodev->dev);

    if (!priv)
        return;

    dsa_switch_shutdown(priv->ds);

    dev_set_drvdata(&mdiodev->dev, NULL);
}

static const struct of_device_id rtl8366ub_mdio_of_match[] = {
    { .compatible = "realtek,rtl8366ub" },
    { /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, rtl8366ub_mdio_of_match);

static struct mdio_driver rtl8366ub_mdio_driver = {
    .mdiodrv.driver = {
        .name = "rtl8366ub-mdio",
        .of_match_table = rtl8366ub_mdio_of_match,
    },
    .probe  = rtl8366ub_mdio_probe,
    .remove = rtl8366ub_mdio_remove,
    .shutdown = rtl8366ub_mdio_shutdown,
};

static int __init rtl8366ub_module_init(void)
{
    int ret;

    ret = rtl8366ub_phy_driver_register();
    if (ret)
        return ret;

    ret = mdio_driver_register(&rtl8366ub_mdio_driver);
    if (ret)
        rtl8366ub_phy_driver_unregister();

    return ret;
}
module_init(rtl8366ub_module_init);

static void __exit rtl8366ub_module_exit(void)
{
    mdio_driver_unregister(&rtl8366ub_mdio_driver);
    rtl8366ub_phy_driver_unregister();
}
module_exit(rtl8366ub_module_exit);

MODULE_AUTHOR("Jianhui Zhao <jianhui.zhao@gl-inet.com>");
MODULE_DESCRIPTION("DSA driver for the RTL8371C (RTL8366UB) 2.5G switch");
MODULE_LICENSE("GPL");
