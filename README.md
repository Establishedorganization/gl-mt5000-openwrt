# gl-mt5000-openwrt

A **clean, minimal** build of **official OpenWrt main** (kernel 6.18) for the
**GL.iNet GL-MT5000 (Brume 3)**, with the RTL8371C ("RTL8366UB") switch driven
as a proper **DSA** switch.

This is *vanilla OpenWrt + the not-yet-merged device support + our fixes*. It is
**not** a GL.iNet firmware fork, and it carries no proxy stack, themes, or
external feeds.

## What changed (2026-08-30)

GL.iNet **force-pushed PR #24237 on 2026-08-27**. It is no longer the swconfig
driver this repo used to convert — GL now ship their own DSA driver plus the
complete Realtek RTK SDK as GPL source (~90k lines), rebased onto OpenWrt main
and kernel 6.18. The old swconfig→DSA conversion in `scripts/diy.sh` is gone;
there is nothing left to convert.

That upstream driver is **bench-validated**: on the GL forum
([thread 67297](https://forum.gl-inet.com/t/gl-mt5000-brume-3-upstream-openwrt-support/67297),
post #123) a tester reports LAN1↔LAN2 and VLAN trunking both working on real
hardware. It is the reference; this repo layers fixes on top of it.

## What the build does

1. Clones official `openwrt/openwrt` at branch `main` (kernel 6.18).
2. Grafts GL's device-support + DSA-driver commit from PR
   [#24237](https://github.com/openwrt/openwrt/pull/24237) — DTS, image recipe,
   board files, the RTL8371C SDK, the `rtl8366ub_phy.c` PHY driver, and the
   `kmod-dsa-tag-rtl8-4` package split.
3. Replaces `rtl8366ub_dsa.c` with our fixed version (below).
4. Adds a MediaTek PPE patch that makes **hardware flow offload** work behind a
   Realtek DSA switch.
5. Builds a minimal image: LuCI + `ethtool`/`ip-full`/`tcpdump`/`iperf3`.

Driven by `scripts/diy.sh` and `config/mt5000.config`.

## Our changes to GL's driver

`files/dsa/rtl8366ub_dsa.c`, layered on GL's:

1. **PHY power up/down actually reaches the chip.** GL writes OCP register
   `0xa610` through `rtk_port_phyReg_set()`, the Clause-22 accessor, which
   rejects any reg > 31 with `RT_ERR_PHY_REG_ID` — so both their power-up and
   power-down writes are silently dropped. We use the OCP accessor and the same
   bit-11 read-modify-write that `dal_rtl8371c_port_phyEnableAll_set()` does.
2. **STP state reaches hardware.** GL writes MSTI 1, which no VLAN entry
   references (they all carry `fid_msti = 0`), making `.port_stp_state_set` a
   no-op. We write MSTI 0.
3. **`.port_vlan_filtering`,** plus clearing the ingress VLAN filter that
   `rtk_vlan_init()` turns on — including on the CPU trunk port, which DSA
   never toggles.
4. **VLAN add/del fail closed** — a failed read never falls back to a zeroed
   entry, which would erase every other member port of that VID.
5. **`.port_fdb_add`/`.port_fdb_del` + `assisted_learning_on_cpu_port`.** The
   rtl8_4 tagger sets LEARN_DIS on every CPU-injected frame, so without this
   the switch never learns the router's own MAC and floods every reply.
6. **`.port_fast_age`, `.port_bridge_flags`, `.port_change_mtu`/`.port_max_mtu`**
   (the last accounting for the 8-byte rtl8_4 tag on the CPU port).
7. **`.shutdown`** and a debugfs corner (`/sys/kernel/debug/rtl8366ub/`) for
   bring-up.

## Hardware flow offload

`files/patches/795-10-mtk_ppe_offload-offload-flows-to-rtl8_4-switches.patch`

`mtk_flow_get_dsa_port()` only substitutes the DSA conduit for tag protocols
the PPE can generate a tag for; everything else returns early with `*dev` still
pointing at the user netdev. `mtk_flow_set_output_device()` then matches it
against none of the GDM netdevs and returns `-EOPNOTSUPP`.

On this board WAN is `eth1` (SoC PHY) and LAN is behind the Realtek switch, so
LAN→WAN offloads fine and WAN→LAN is refused. A flowtable entry needs both
directions, so hardware offload silently degrades to the software path — which
is exactly the reported symptom: ~2.25 Gbit/s with one core pinned at 100%,
versus 2.37 Gbit/s near-idle on vendor firmware (forum #123).

The PPE cannot synthesise the Realtek 0x8899 tag, but it does not need to:
`rtk_cpu_tagPort_set()` programs `CPU_TAG_AWARE_CTRL`, and the switch parses an
inbound CPU tag only when that Length/Type is actually present. An untagged
PPE-injected frame takes the switch's ordinary FDB path. The patch substitutes
the conduit and returns `-ENODEV`, so the caller skips `mtk_foe_entry_set_dsa()`
and the DSA queue map.

It builds on OpenWrt's `795-09` (Daniel Golle), which already generalised this
function for the MaxLinear MxL862xx — the same problem class.

The same substitution also corrects ingress `ppe_index` selection, but that is
a no-op on this board: MT7987 has `ppe_num = 2` and PPE index follows MAC id,
so the conduit `gmac0` maps to `ppe_idx 0`, which is already the default
`mtk_eth_setup_tc_block_cb()` passes. The patch's real effect here is
unblocking the WAN→LAN egress direction.

**Provenance:** GL acknowledged the offload problem (forum #126), shipped a
fixed test build via WeTransfer (#130) that a tester confirmed working (#134),
and said it would be committed "once validation completes" (#135, 2026-08-29).
As of 2026-08-30 it is **not** in any branch of their repo. This patch is our
independently derived equivalent — **drop it once GL publishes theirs**, since
they have bench-validated theirs and we have not.

## Status / caveats

- ⚠️ **Not hardware-validated by us.** `diy.sh` runs clean end to end and the
  PPE patch applies at fuzz 0 against 6.18.44 + OpenWrt's generic patches, but
  nothing here has been booted on a unit.
- ⚠️ **The MSTI-0 change is the riskiest delta.** It takes a call that
  currently does nothing and makes it program real forwarding state. If ports
  come up blocked, revert that first.
- ⚠️ **VLAN + hardware offload.** An untagged PPE-injected frame takes the CPU
  port's PVID. Flows egressing a VLAN upper (e.g. `br-lan.20`) still carry
  their user VLAN push and land correctly; a tagged trunk with no user VLAN
  push is the case to watch.
- ❓ **`reset-gpios` is still `<&pio 42>`.** A dump from a retail unit running
  GL's own firmware puts the switch reset on **48**
  ([PR #24237 comment, 2026-08-01](https://github.com/openwrt/openwrt/pull/24237)),
  and nobody replied. GL changed only the polarity flag in the force-push
  (`ACTIVE_HIGH`→`ACTIVE_LOW`). Since the switch demonstrably works as shipped,
  we leave GL's DTS untouched — but the pin is still unexplained.
- This is a **fork-only** solution. Mainline wants the RTL8371C added to the
  existing `rtl8365mb` driver rather than vendoring GL's SDK. This repo
  prioritises a *working image now*.

## Build it

GitHub → **Actions** → **Build GL-MT5000 OpenWrt (DSA)** → **Run workflow**.
Download the **`gl-mt5000-firmware`** artifact (kept 14 days):
- `openwrt-mediatek-filogic-glinet_gl-mt5000-squashfs-sysupgrade.bin` — flash image
- `...-initramfs-kernel.bin` — RAM-boot kernel for safe serial/U-Boot testing
- `resolved.config`, `.manifest` — what was actually built

## Install / recovery

- **Flash:** upload the `sysupgrade.bin` on the GL stock firmware upgrade page
  (uncheck "keep settings"), or `sysupgrade -n <image>` over SSH.
- **Recovery:** GL U-Boot failsafe — power on holding reset until the LED
  flashes, browse to `192.168.1.1`, upload stock GL firmware.
  See <https://docs.gl-inet.com/router/en/4/faq/debrick/>.

## Layout

```
.github/workflows/build.yml    cloud build (GitHub Actions)
scripts/diy.sh                 graft + apply our driver and PPE patch
config/mt5000.config           lean seed .config
files/dsa/rtl8366ub_dsa.c      DSA driver (GL's + our fixes)
files/patches/795-10-*.patch   PPE hardware flow offload for rtl8_4
```

The DTS is no longer overridden — GL's is used as-is.
