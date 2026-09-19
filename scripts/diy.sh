#!/usr/bin/env bash
# Graft GL.iNet's GL-MT5000 device support (OpenWrt PR #24237) onto official
# openwrt main, then layer our RTL8371C driver fixes and the MediaTek PPE
# hardware-flow-offload patch on top.
#
# As of GL's 2026-08-27 force-push, PR #24237 IS a DSA driver and ships the
# full Realtek RTK SDK as GPL source, so there is no swconfig->DSA conversion
# left to do here - we only replace rtl8366ub_dsa.c with our fixed version and
# add the PPE patch. Everything else (DTS, PHY driver, board.d, image recipe,
# kmod-dsa-tag-rtl8-4 split) comes from GL's commit unmodified.
#
# Run with CWD = OpenWrt source root.
set -eu

WORKSPACE="${GITHUB_WORKSPACE:-$(cd "$(dirname "$0")/.." && pwd)}"
RTLPKG=package/kernel/rtl8366ub
BOARDD=target/linux/mediatek/filogic/base-files/etc/board.d/02_network
FILOGIC_MK=target/linux/mediatek/image/filogic.mk
GENERIC_PATCHES=target/linux/generic/pending-6.18
DTS=target/linux/mediatek/dts/mt7987a-gl-mt5000.dts

# --- 1. Graft the GL device-support commit (PR #24237 head) ---------------
echo ">> Grafting GL-MT5000 device support (PR #24237) onto openwrt main"
git config user.email build@local
git config user.name mt5000-build
git remote add glinet "${GL_DEVICE_COMMIT:-https://github.com/GLiNet-Tech/openwrt.git}" 2>/dev/null || true
# depth>=2 so cherry-pick has the commit's PARENT as a merge base; with depth 1
# git lacks the base and treats the whole tree as add/add conflicts.
git fetch --depth 3 glinet mt5000
git cherry-pick -n FETCH_HEAD || {
	echo ">> ERROR: graft cherry-pick failed (openwrt main drift?)"
	git cherry-pick --abort 2>/dev/null || true
	exit 1
}
test -f "$DTS" || { echo ">> ERROR: DTS missing after graft"; exit 1; }
grep -q "glinet_gl-mt5000" "$FILOGIC_MK" || { echo ">> ERROR: device recipe missing after graft"; exit 1; }
echo ">> graft OK"

# --- 2. Our RTL8371C driver on top of GL's -------------------------------
echo ">> Installing patched rtl8366ub_dsa.c"

src="$WORKSPACE/files/dsa/rtl8366ub_dsa.c"
dst="$RTLPKG/src/rtl8366ub_dsa.c"

if [[ ! -f "$src" ]]; then
    echo "ERROR: missing patched driver source: $src"
    exit 1
fi

mkdir -p "$(dirname "$dst")"
cp "$src" "$dst"

# Our driver keeps a debugfs dir handle in priv; GL's header has no such field.
if ! grep -q 'struct dentry \*dbgfs;' "$RTLPKG/src/rtl8366ub_dsa.h"; then
	# perl, not sed -i: BSD/macOS sed rejects both bare -i and \n in the RHS,
	# and this script is run by hand as often as it is run by CI.
	perl -0pi -e 's|^( *)struct mutex reg_mutex;|$1struct dentry *dbgfs;\n\n$1struct mutex reg_mutex;|m' \
		"$RTLPKG/src/rtl8366ub_dsa.h"
fi

# --- 3. MediaTek PPE hardware flow-offload for rtl8_4 --------------------
# GL confirmed on their forum (thread 67297, #126) that HW acceleration is
# "not fully compatible" on vanilla OpenWrt, shipped a fixed test build in
# #130 that a tester confirmed working in #134, and said in #135 it would be
# committed "once validation completes" - it is not in the repo yet. This is
# our independently derived equivalent; drop it once GL publishes theirs.
echo ">> Adding PPE rtl8_4 flow-offload patch"
cp "$WORKSPACE/files/patches/795-10-mtk_ppe_offload-offload-flows-to-rtl8_4-switches.patch" \
	"$GENERIC_PATCHES/"

# --- 4. Sanity gates (each can actually fail) ----------------------------
grep -q "rtl8366ub_dsa.o" "$RTLPKG/src/Makefile" \
	|| { echo ">> ERROR: DSA object not wired into src/Makefile"; exit 1; }
grep -q "rtl8366ub_phy.o" "$RTLPKG/src/Makefile" \
	|| { echo ">> ERROR: PHY driver not wired in - 2.5G link + link state depend on it"; exit 1; }
grep -q "ccflags-y += -I\$(src)" "$RTLPKG/src/Makefile" \
	|| { echo ">> ERROR: -I\$(src) missing; chip.c will fail on <rtk_error.h>"; exit 1; }
grep -q "phylink_mac_ops" "$RTLPKG/src/rtl8366ub_dsa.c" \
	|| { echo ">> ERROR: our driver did not land (no phylink_mac_ops)"; exit 1; }
grep -q "port_vlan_filtering" "$RTLPKG/src/rtl8366ub_dsa.c" \
	|| { echo ">> ERROR: our driver did not land (no .port_vlan_filtering)"; exit 1; }
grep -q 'struct dentry \*dbgfs;' "$RTLPKG/src/rtl8366ub_dsa.h" \
	|| { echo ">> ERROR: dbgfs field not injected into priv struct"; exit 1; }
grep -q "kmod-dsa-tag-rtl8-4" package/kernel/linux/modules/netdevices.mk \
	|| { echo ">> ERROR: kmod-dsa-tag-rtl8-4 package missing"; exit 1; }
grep -q "DSA_TAG_PROTO_RTL8_4" "$GENERIC_PATCHES/795-10-mtk_ppe_offload-offload-flows-to-rtl8_4-switches.patch" \
	|| { echo ">> ERROR: PPE patch not installed"; exit 1; }
grep -q 'glinet,gl-mt5000)' "$BOARDD" \
	|| { echo ">> ERROR: gl-mt5000 case missing from board.d"; exit 1; }
sh -n "$BOARDD" || { echo ">> ERROR: board.d/02_network has a shell syntax error"; exit 1; }

# --- 5. First-boot defaults ----------------------------------------------
mkdir -p files/etc/uci-defaults
cat > files/etc/uci-defaults/99-gl-mt5000 <<'UCI'
#!/bin/sh
uci -q batch <<-EOF
	set system.@system[0].hostname='GL-MT5000'
	set system.@system[0].timezone='WET0WEST,M3.5.0/1,M10.5.0'
	set system.@system[0].zonename='Europe/Lisbon'
	commit system
EOF
exit 0
UCI

echo ">> diy.sh complete"
