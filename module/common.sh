#!/system/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# YukiZygisk module helpers for selecting a KMI-specific kernel module.
#
# License: Apache-2.0
#
# Author: Anatdx

yz_detect_kmi() {
	yz_release="${1:-$(uname -r 2>/dev/null)}"
	yz_android="$(printf '%s\n' "$yz_release" |
		sed -n 's/.*\(android[0-9][0-9]*\).*/\1/p')"
	yz_kernel="$(printf '%s\n' "$yz_release" |
		sed -n 's/^\([0-9][0-9]*\.[0-9][0-9]*\).*/\1/p')"

	[ -n "$yz_android" ] && [ -n "$yz_kernel" ] || return 1
	printf '%s-%s\n' "$yz_android" "$yz_kernel"
}

yz_kmi_ko() {
	yz_moddir="$1"
	yz_kmi="$2"
	printf '%s/lkm/%s_yukizygisk.ko\n' "$yz_moddir" "$yz_kmi"
}

yz_list_supported_kmis() {
	yz_moddir="$1"
	yz_found=false
	for yz_ko in "$yz_moddir"/lkm/*_yukizygisk.ko; do
		[ -f "$yz_ko" ] || continue
		yz_name="${yz_ko##*/}"
		printf '%s\n' "${yz_name%_yukizygisk.ko}"
		yz_found=true
	done
	[ "$yz_found" = true ]
}
