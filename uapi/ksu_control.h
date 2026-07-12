/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0 */
/*
 * YukiZygisk - Shared userspace contract for the integrated (built-in)
 * KernelSU control-fd handoff.
 *
 * In built-in mode the daemon (zygiskd) and the control client (yzctl) obtain
 * the anonymous YukiZygisk control fd through the host KernelSU: they open the
 * KSU driver fd via the reboot-syscall install channel, then request the YZ
 * control fd with an ioctl on it. These constants are the single source of
 * truth shared by both binaries; they must stay in sync with the host KernelSU
 * (ReSukiSU/SukiSU) uapi/supercall.h, where:
 *   - KSU_INSTALL_MAGIC1 / KSU_INSTALL_MAGIC2 gate __NR_reboot fd install
 *   - KSU_IOCTL_YZ_INSTALL_FD is DEFINE_KSU_UAPI_CONST(__u32, ...,
 *     _IOR('K', 0x50, __s32)) and dispatches to yukizygisk_integrated_install_fd().
 *
 * License: Author's work under Apache-2.0; when used as a kernel module
 * (or linked with the Linux kernel), GPL-2.0 applies for kernel compatibility.
 *
 * Author: Anatdx
 */
#pragma once

#include <cstdint>
#include <sys/ioctl.h>

#ifndef KSU_INSTALL_MAGIC1
#define KSU_INSTALL_MAGIC1 0xDEADBEEFu
#endif
#ifndef KSU_INSTALL_MAGIC2
#define KSU_INSTALL_MAGIC2 0xCAFEBABEu
#endif
#ifndef KSU_IOCTL_YZ_INSTALL_FD
#define KSU_IOCTL_YZ_INSTALL_FD _IOR('K', 0x50, int32_t)
#endif
