/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0 */
/*
 * YukiZygisk - Integrated entry points exposed to the host KernelSU.
 * Author: Anatdx
 */
#ifndef _YUKIZYGISK_INTEGRATED_H
#define _YUKIZYGISK_INTEGRATED_H

/* Bring up / tear down the whole YukiZygisk kernel side. Never fails hard:
 * on error it deactivates its own stages and returns, so KSU boot is safe. */
void yukizygisk_kernel_init(void);
void yukizygisk_kernel_exit(void);

/*
 * Stable host API: create the anonymous YZ control fd for the calling process
 * and return it (>= 0) or a negative errno. The host KernelSU's
 * KSU_IOCTL_YZ_INSTALL_FD dispatch handler calls exactly this, so the ABI is
 * insulated from the internal control-fd signature.
 *
 * It installs a non-bootstrap control fd: integrated builds exclude the LKM
 * bootstrap (core/bootstrap.o) and arm no fail-closed guard, so the fd is not a
 * bootstrap fd and the userspace daemon skips the YZ_IOCTL_DAEMON_READY
 * handshake in this mode. LKM bootstrap behaviour is unaffected.
 */
int yukizygisk_integrated_install_fd(void);

#endif /* _YUKIZYGISK_INTEGRATED_H */
