/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0 */
/*
 * YukiZygisk - Anonymous ioctl control file declarations.
 *
 * License: Author's work under Apache-2.0; when used as a kernel module
 * (or linked with the Linux kernel), GPL-2.0 applies for kernel compatibility.
 *
 * Author: Anatdx
 */
#ifndef _YUKIZYGISK_CONTROL_H
#define _YUKIZYGISK_CONTROL_H

int yukizygisk_control_init(void);
void yukizygisk_control_exit(void);
int yukizygisk_control_install_fd(void);

#endif /* _YUKIZYGISK_CONTROL_H */
