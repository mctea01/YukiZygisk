/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0 */
/*
 * YukiZygisk - Root-implementation agnostic host adapter.
 *
 * License: Author's work under Apache-2.0; when used as a kernel module
 * (or linked with the Linux kernel), GPL-2.0 applies for kernel compatibility.
 *
 * Author: Anatdx
 */

#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/file.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/string.h>

#include "host/host.h"
#include "host/policy.h"
#include "host/root_impl.h"
#include "host/runtime.h"

static DEFINE_MUTEX(yz_feature_lock);
static const struct yz_host_feature_handler
	*yz_feature_handlers[YZ_FEATURE_MAX + 1];
static struct cred *yz_priv_cred;

int yz_host_init(void)
{
	int ret;

	ret = yz_host_runtime_init();
	if (ret)
		return ret;
	ret = yz_host_policy_init();
	if (ret) {
		yz_host_runtime_exit();
		return ret;
	}

	yz_priv_cred = prepare_creds();
	if (!yz_priv_cred) {
		yz_host_policy_exit();
		yz_host_runtime_exit();
		return -ENOMEM;
	}

	pr_info("yukizygisk: host roots mask=0x%x policy=%s\n", yz_root_mask,
		yz_host_root_allows_policy() ? "available" : "unavailable");
	return 0;
}

void yz_host_exit(void)
{
	if (yz_priv_cred) {
		abort_creds(yz_priv_cred);
		yz_priv_cred = NULL;
	}
	yz_host_policy_exit();
	yz_host_runtime_exit();
}

int yz_host_register_feature_handler(
	const struct yz_host_feature_handler *handler)
{
	if (!handler || handler->feature_id > YZ_FEATURE_MAX)
		return -EINVAL;

	mutex_lock(&yz_feature_lock);
	if (yz_feature_handlers[handler->feature_id]) {
		mutex_unlock(&yz_feature_lock);
		return -EEXIST;
	}
	yz_feature_handlers[handler->feature_id] = handler;
	mutex_unlock(&yz_feature_lock);

	pr_info("yukizygisk: feature registered id=%u name=%s\n",
		handler->feature_id, handler->name ?: "(null)");
	return 0;
}

int yz_host_unregister_feature_handler(u32 feature_id)
{
	if (feature_id > YZ_FEATURE_MAX)
		return -EINVAL;

	mutex_lock(&yz_feature_lock);
	yz_feature_handlers[feature_id] = NULL;
	mutex_unlock(&yz_feature_lock);
	return 0;
}

int yz_host_get_feature(u32 feature_id, u64 *value, bool *supported)
{
	const struct yz_host_feature_handler *handler;
	int ret = 0;

	if (value)
		*value = 0;
	if (supported)
		*supported = false;
	if (feature_id > YZ_FEATURE_MAX)
		return -EINVAL;

	mutex_lock(&yz_feature_lock);
	handler = yz_feature_handlers[feature_id];
	if (handler && handler->get_handler) {
		if (supported)
			*supported = true;
		ret = handler->get_handler(value);
	}
	mutex_unlock(&yz_feature_lock);
	return handler ? ret : -ENOENT;
}

int yz_host_set_feature(u32 feature_id, u64 value)
{
	const struct yz_host_feature_handler *handler;
	int ret;

	if (feature_id > YZ_FEATURE_MAX)
		return -EINVAL;

	mutex_lock(&yz_feature_lock);
	handler = yz_feature_handlers[feature_id];
	if (!handler || !handler->set_handler) {
		mutex_unlock(&yz_feature_lock);
		return -ENOENT;
	}
	ret = handler->set_handler(value);
	mutex_unlock(&yz_feature_lock);
	return ret;
}

const struct cred *yz_host_override_creds(void)
{
	return yz_priv_cred ? override_creds(yz_priv_cred) : NULL;
}

void yz_host_revert_creds(const struct cred *old_cred)
{
	if (old_cred)
		revert_creds(old_cred);
}

bool yz_host_is_zygote(const struct cred *cred)
{
	return yz_host_policy_cred_has_type(cred, "zygote");
}

int yz_host_file_load_policy_allow_current(
	struct file *file, struct yz_file_load_policy *state)
{
	return yz_host_policy_allow_file_current(file, state);
}

int yz_host_file_load_policy_allow_execmem_current(
	struct yz_file_load_policy *state)
{
	return yz_host_policy_allow_execmem_current(state);
}

int yz_host_file_load_policy_restore(const struct yz_file_load_policy *state)
{
	return yz_host_policy_restore(state);
}
