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

/* Create the anonymous YZ control fd for the calling process and return it
 * (>= 0) or a negative errno. Delivered via KSU_IOCTL_YZ_INSTALL_FD. */
int yukizygisk_control_install_fd(void);

#endif /* _YUKIZYGISK_INTEGRATED_H */
