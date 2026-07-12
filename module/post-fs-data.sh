#!/system/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# YukiZygisk module post-fs-data entry point.
#
# License: Apache-2.0
#
# One package supports both kernels:
#   - Integrated (built-in) kernel: zygiskd claims the control fd through the
#     KernelSU ioctl channel. NO insmod is performed.
#   - Non-integrated kernel: the matching LKM is insmod'd with a per-boot
#     bootstrap cookie, then zygiskd falls back to the prctl bootstrap.
#
# The integrated-vs-LKM decision is made by a single source of truth:
# `zygiskd --probe-integrated` (exit 0 = integrated path works).
#
# Author: Anatdx

MODDIR="${0%/*}"
BASE_DIR="/data/adb/yukizygisk"
LIB_DIR="$BASE_DIR/lib"
RUN_DIR="$BASE_DIR/run"
LOG_FILE="$BASE_DIR/zygiskd.log"
CONFIG_FILE="$BASE_DIR/yzconfig.json"
MODULES_DIR="/data/adb/modules"

mkdir -p "$BASE_DIR" "$LIB_DIR" "$RUN_DIR"
chmod 0755 "$BASE_DIR" "$LIB_DIR" "$RUN_DIR"
touch "$LOG_FILE" 2>/dev/null || true

log() { echo "post-fs-data: $*" >>"$LOG_FILE"; }

if [ ! -f "$CONFIG_FILE" ]; then
	printf '%s\n' '{"yukilinker":true,"denylist_mode":0,"dmesg_log":false}' \
		>"$CONFIG_FILE"
fi
chmod 0600 "$CONFIG_FILE" 2>/dev/null || true

# ---- deploy userspace payload ----
chmod 0755 "$MODDIR/zygiskd" 2>/dev/null || true
for lib in libzygisk.so libyukilinker.so libyukizncore.so; do
	if [ ! -f "$MODDIR/$lib" ]; then
		log "missing payload $lib"
		exit 0
	fi
	if ! cp "$MODDIR/$lib" "$LIB_DIR/$lib.tmp" 2>>"$LOG_FILE"; then
		log "copy $lib failed"
		exit 0
	fi
	mv "$LIB_DIR/$lib.tmp" "$LIB_DIR/$lib" 2>>"$LOG_FILE" || exit 0
	chmod 0644 "$LIB_DIR/$lib" 2>/dev/null || true
done

# ---- probe: is the running kernel integrated? ----
INTEGRATED=0
if "$MODDIR/zygiskd" --probe-integrated >>"$LOG_FILE" 2>&1; then
	INTEGRATED=1
	log "integrated kernel detected; skipping insmod"
else
	log "integrated path unavailable; will use LKM fallback"
fi

COOKIE=""

if [ "$INTEGRATED" -eq 0 ]; then
	# ---- LKM fallback path (original behavior) ----
	if [ ! -f "$MODDIR/common.sh" ]; then
		log "missing module KMI helpers; cannot use LKM path"
		exit 0
	fi
	# shellcheck source=/dev/null
	. "$MODDIR/common.sh"

	KERNEL_RELEASE="$(uname -r 2>/dev/null)"
	if ! KMI="$(yz_detect_kmi "$KERNEL_RELEASE")"; then
		log "cannot detect GKI KMI from: $KERNEL_RELEASE"
		exit 0
	fi
	KERNEL_MODULE="$(yz_kmi_ko "$MODDIR" "$KMI")"
	if [ ! -f "$KERNEL_MODULE" ]; then
		SUPPORTED="$(yz_list_supported_kmis "$MODDIR" | tr '\n' ' ')"
		log "missing kernel module for $KMI; available: ${SUPPORTED:-none}"
		exit 0
	fi

	random_cookie() {
		c="$(od -An -N8 -tx8 /dev/urandom 2>/dev/null | tr -d ' \n')"
		if [ -n "$c" ]; then echo "0x$c"; else echo "0x$(date +%s)$$"; fi
	}
	COOKIE="$(random_cookie)"

	if grep -q '^yukizygisk ' /proc/modules 2>/dev/null; then
		log "yukizygisk.ko already loaded"
	else
		INSMOD="$(command -v insmod 2>/dev/null || echo /system/bin/insmod)"
		KSU_MODULE_PRESENT=0
		if yz_ksu_module_loaded; then KSU_MODULE_PRESENT=1; fi
		log "insmod $KERNEL_MODULE ($KMI) cookie=$COOKIE ksu_present=$KSU_MODULE_PRESENT"
		if ! "$INSMOD" "$KERNEL_MODULE" bootstrap_cookie_lo="$COOKIE" \
			ksu_module_present="$KSU_MODULE_PRESENT" >>"$LOG_FILE" 2>&1; then
			log "insmod failed; zygiskd will still try integrated path"
			COOKIE=""
		fi
	fi
fi

# ---- start zygiskd (dual-path: tries integrated first, then bootstrap) ----
log "starting zygiskd (integrated=$INTEGRATED)"
if [ -n "$COOKIE" ]; then
	YUKIZYGISK_BOOTSTRAP_COOKIE_LO="$COOKIE" \
	YUKIZYGISK_CONFIG="$CONFIG_FILE" \
	YUKIZYGISK_MODULES_DIR="$MODULES_DIR" \
	"$MODDIR/zygiskd" \
		--bootstrap-cookie-lo "$COOKIE" \
		--config "$CONFIG_FILE" \
		--modules-dir "$MODULES_DIR" \
		>>"$LOG_FILE" 2>&1 &
else
	YUKIZYGISK_CONFIG="$CONFIG_FILE" \
	YUKIZYGISK_MODULES_DIR="$MODULES_DIR" \
	"$MODDIR/zygiskd" \
		--config "$CONFIG_FILE" \
		--modules-dir "$MODULES_DIR" \
		>>"$LOG_FILE" 2>&1 &
fi

exit 0
