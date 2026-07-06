/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0 */
/*
 * YukiZygisk - Host integration adapter declarations.
 *
 * License: Author's work under Apache-2.0; when used as a kernel module
 * (or linked with the Linux kernel), GPL-2.0 applies for kernel compatibility.
 *
 * Author: Anatdx
 */
#ifndef _YUKIZYGISK_HOST_H
#define _YUKIZYGISK_HOST_H

#include <linux/cred.h>
#include <linux/types.h>

#include "host/lsm.h"

#define YZ_FEATURE_YUKIZYGISK 1
#define YZ_FEATURE_MAX 32

struct file;

typedef int (*yz_host_feature_get_t)(u64 *value);
typedef int (*yz_host_feature_set_t)(u64 value);

struct yz_host_feature_handler {
	u32 feature_id;
	const char *name;
	yz_host_feature_get_t get_handler;
	yz_host_feature_set_t set_handler;
};

struct yz_file_load_policy {
	u32 src_type;
	u32 tgt_type;
	u32 tmpfs_type;
	u32 process_type;
	u16 target_class;
	u16 process_class;
	u32 added_av;
	u32 tmpfs_added_av;
	u32 process_added_av;
};

int yz_host_init(void);
void yz_host_exit(void);

int yz_host_register_feature_handler(
	const struct yz_host_feature_handler *handler);
int yz_host_unregister_feature_handler(u32 feature_id);
int yz_host_get_feature(u32 feature_id, u64 *value, bool *supported);
int yz_host_set_feature(u32 feature_id, u64 value);

const struct cred *yz_host_override_creds(void);
void yz_host_revert_creds(const struct cred *old_cred);

bool yz_host_is_zygote(const struct cred *cred);

int yz_host_file_load_policy_allow_current(
	struct file *file, struct yz_file_load_policy *state);
int yz_host_file_load_policy_allow_execmem_current(
	struct yz_file_load_policy *state);
int yz_host_file_load_policy_restore(const struct yz_file_load_policy *state);

#endif /* _YUKIZYGISK_HOST_H */
