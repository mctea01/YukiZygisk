/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0 */
/*
 * YukiZygisk - kernel-side orchestrator: per-app process lifecycle state
 * machine.
 *
 * License: Author's work under Apache-2.0; when used as a kernel module
 * (or linked with the Linux kernel), GPL-2.0 applies for kernel compatibility.
 *
 * Author: Anatdx
 */
#ifndef __YZ_H_ZYGOTE_ORCH
#define __YZ_H_ZYGOTE_ORCH

#include <linux/types.h>

void yz_zygote_orch_init(void);
void yz_zygote_orch_exit(void);

/* Fed by the setresuid hook to learn a tracked child's identity. */
void yz_zygote_orch_on_setresuid(uid_t old_uid, uid_t new_uid);

#endif // #ifndef __YZ_H_ZYGOTE_ORCH
