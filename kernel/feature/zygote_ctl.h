/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0 */
/*
 * YukiZygisk - kernel control plane: zygiskd -> kernel handoff + fd brokering.
 *
 * License: Author's work under Apache-2.0; when used as a kernel module
 * (or linked with the Linux kernel), GPL-2.0 applies for kernel compatibility.
 *
 * Author: Anatdx
 */
#ifndef __YZ_H_ZYGOTE_CTL
#define __YZ_H_ZYGOTE_CTL

#include <linux/types.h>

int yz_zygote_ctl_handoff(void __user *arg);
void yz_zygote_ctl_release(pid_t pid);
void yz_zygote_ctl_init(void);
void yz_zygote_ctl_exit(void);

#endif /* __YZ_H_ZYGOTE_CTL */
