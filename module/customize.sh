#!/system/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# YukiZygisk module install hook.
#
# License: Apache-2.0
#
# Author: Anatdx

ui_print "- Installing YukiZygisk"

[ -f "$MODPATH/common.sh" ] || abort "! Missing module KMI helpers"
# shellcheck source=/dev/null
. "$MODPATH/common.sh"

ABI="$(getprop ro.product.cpu.abi 2>/dev/null)"
case "$ABI" in
arm64-v8a) ;;
*) abort "! Unsupported ABI for this package: $ABI" ;;
esac

KERNEL_RELEASE="$(uname -r 2>/dev/null)"
KMI="$(yz_detect_kmi "$KERNEL_RELEASE")" ||
	abort "! Cannot detect GKI KMI from kernel release: $KERNEL_RELEASE"
KERNEL_MODULE="$(yz_kmi_ko "$MODPATH" "$KMI")"
if [ ! -f "$KERNEL_MODULE" ]; then
	SUPPORTED_KMIS="$(yz_list_supported_kmis "$MODPATH" | tr '\n' ' ')"
	abort "! Package has no module for $KMI (available: ${SUPPORTED_KMIS:-none})"
fi
[ -f "$MODPATH/zygiskd" ] || abort "! Missing zygiskd"
for lib in libzygisk.so libyukilinker.so libyukizncore.so; do
	[ -f "$MODPATH/$lib" ] || abort "! Missing $lib"
done

chmod 0644 "$MODPATH"/lkm/*.ko "$MODPATH/common.sh"
chmod 0644 "$MODPATH"/lib*.so
chmod 0755 "$MODPATH/zygiskd" "$MODPATH/post-fs-data.sh" \
	"$MODPATH/boot-completed.sh" "$MODPATH/action.sh"

BASE_DIR="/data/adb/yukizygisk"
mkdir -p "$BASE_DIR/lib"
chmod 0755 "$BASE_DIR" "$BASE_DIR/lib"

ui_print "- Selected kernel module: $KMI"
ui_print "- YukiZygisk installed"
