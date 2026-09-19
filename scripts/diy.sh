#!/usr/bin/env bash
set -euo pipefail

WORKSPACE="${GITHUB_WORKSPACE:-$(cd "$(dirname "$0")/.." && pwd)}"
FILOGIC_MK=target/linux/mediatek/image/filogic.mk
DTS=target/linux/mediatek/dts/mt7987a-gl-mt5000.dts
DRIVER_PATCH=target/linux/generic/pending-6.18/795-10-net-dsa-realtek-add-rtl8366ub.patch
PPE_PATCH="$WORKSPACE/files/patches/795-10-mtk_ppe_offload-offload-flows-to-rtl8_4-switches.patch"
PPE_DEST=target/linux/generic/pending-6.18

echo ">> Grafting GL-MT5000 device support (PR #24237) onto OpenWrt main"

git config user.email build@local
git config user.name mt5000-build
git remote add glinet \
    "${GL_DEVICE_COMMIT:-https://github.com/GLiNet-Tech/openwrt.git}" \
    2>/dev/null || true

git fetch --depth 3 glinet mt5000

git cherry-pick -n FETCH_HEAD || {
    echo ">> ERROR: graft cherry-pick failed"
    git cherry-pick --abort 2>/dev/null || true
    exit 1
}

test -f "$DTS" || {
    echo ">> ERROR: MT5000 DTS missing after graft: $DTS"
    exit 1
}

grep -q "glinet_gl-mt5000" "$FILOGIC_MK" || {
    echo ">> ERROR: MT5000 device recipe missing after graft"
    exit 1
}

test -f "$DRIVER_PATCH" || {
    echo ">> ERROR: GL RTL8366UB kernel driver patch missing: $DRIVER_PATCH"
    exit 1
}

echo ">> GL DSA graft OK"
echo ">> RTL8366UB is supplied by the kernel Realtek DSA patch"

# Keep this only if the PPE patch has been rebased to the current
# DSA_TAG_PROTO_RTL8366UB_8021Q implementation.
if [[ -f "$PPE_PATCH" ]]; then
    if grep -q "DSA_TAG_PROTO_RTL8_4" "$PPE_PATCH"; then
        echo ">> WARNING: skipping obsolete rtl8_4 PPE patch"
        echo ">> Current GL driver uses DSA_TAG_PROTO_RTL8366UB_8021Q"
    else
        cp "$PPE_PATCH" "$PPE_DEST/"
        echo ">> PPE patch installed"
    fi
fi

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
