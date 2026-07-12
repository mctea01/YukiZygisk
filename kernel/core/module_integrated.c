/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0 */
/*
 * YukiZygisk - Integrated (built-in) entry point for ReSukiSU/SukiSU.
 *
 * This replaces the standalone LKM entry. There is no module_init/exit and
 * no prctl+cookie bootstrap; the control fd is delivered through the host
 * KernelSU ioctl channel (KSU_IOCTL_YZ_INSTALL_FD -> yukizygisk_control_install_fd).
 *
 * License: Author's work under Apache-2.0; when linked with the Linux kernel,
 * GPL-2.0 applies for kernel compatibility.
 *
 * Author: Anatdx
 */

#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/printk.h>

#include "core/control.h"
#include "core/lifecycle.h"
#include "feature/zygote_ctl.h"
#include "feature/zygote_nl.h"
#include "feature/zygote_orch.h"
#include "feature/zygote_probe.h"
#include "host/host.h"
#include "uapi/yukizygisk.h"

static bool yz_stage_probe_active;
static bool yz_stage_nl_active;
static bool yz_stage_orch_active;
static bool yz_stage_ctl_active;
static bool yz_stage_control_active;
static bool yz_stage_lsm_active;
static bool yz_stage_host_active;
static DEFINE_MUTEX(yz_lifecycle_lock);

static void yukizygisk_deactivate_locked(void)
{
	if (yz_stage_control_active) {
		yukizygisk_control_exit();
		yz_stage_control_active = false;
	}
	if (yz_stage_ctl_active) {
		yz_zygote_ctl_exit();
		yz_stage_ctl_active = false;
	}
	if (yz_stage_orch_active) {
		yz_zygote_orch_exit();
		yz_stage_orch_active = false;
	}
	if (yz_stage_nl_active) {
		yz_zygote_nl_exit();
		yz_stage_nl_active = false;
	}
	if (yz_stage_probe_active) {
		yz_zygote_probe_exit();
		yz_stage_probe_active = false;
	}
	if (yz_stage_lsm_active) {
		yz_host_lsm_exit();
		yz_stage_lsm_active = false;
	}
	if (yz_stage_host_active) {
		yz_host_exit();
		yz_stage_host_active = false;
	}
}

/*
 * Retained for source compatibility with feature/ files that still reference
 * the lifecycle fail-closed hook. In integrated mode it simply tears down the
 * active stages and stays resident (a built-in cannot self-unload).
 */
void yukizygisk_bootstrap_fail_closed(void)
{
	mutex_lock(&yz_lifecycle_lock);
	yukizygisk_deactivate_locked();
	mutex_unlock(&yz_lifecycle_lock);
}

void yukizygisk_kernel_init(void)
{
	int ret;

	mutex_lock(&yz_lifecycle_lock);

	pr_info("yukizygisk: integrated init start\n");

	ret = yz_host_init();
	if (ret) {
		pr_err("yukizygisk: host init failed: %d\n", ret);
		goto out_unlock;
	}
	yz_stage_host_active = true;

	yz_host_lsm_init();
	yz_stage_lsm_active = true;

	yz_zygote_probe_init();
	yz_stage_probe_active = true;

	yz_zygote_nl_init();
	yz_stage_nl_active = true;
	if (yz_host_policy_uses_fallback()) {
		struct yz_host_root_status status = { 0 };

		yz_host_get_root_status(&status);
		yz_zygote_nl_emit_policy_refresh(status.owner,
						 YZ_POLICY_REFRESH_ALL);
	}

	yz_zygote_orch_init();
	yz_stage_orch_active = true;

	yz_zygote_ctl_init();
	yz_stage_ctl_active = true;

	ret = yukizygisk_control_init();
	if (ret) {
		pr_err("yukizygisk: control init failed: %d\n", ret);
		yukizygisk_deactivate_locked();
		goto out_unlock;
	}
	yz_stage_control_active = true;

	pr_info("yukizygisk: integrated init done\n");

out_unlock:
	mutex_unlock(&yz_lifecycle_lock);
}

void yukizygisk_kernel_exit(void)
{
	mutex_lock(&yz_lifecycle_lock);
	yukizygisk_deactivate_locked();
	mutex_unlock(&yz_lifecycle_lock);
}
