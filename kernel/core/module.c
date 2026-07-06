/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0 */
/*
 * YukiZygisk - Standalone LKM entry point.
 *
 * License: Author's work under Apache-2.0; when used as a kernel module
 * (or linked with the Linux kernel), GPL-2.0 applies for kernel compatibility.
 *
 * Author: Anatdx
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/printk.h>

#include "core/control.h"
#include "feature/zygote_ctl.h"
#include "feature/zygote_nl.h"
#include "feature/zygote_orch.h"
#include "feature/zygote_probe.h"
#include "host/host.h"

static bool yz_enable;
module_param_named(enable, yz_enable, bool, 0644);
MODULE_PARM_DESC(enable, "Enable YukiZygisk feature state on module load");

static int __init yukizygisk_init(void)
{
	int ret;

	pr_info("yukizygisk: standalone LKM initializing\n");

	ret = yz_host_init();
	if (ret)
		return ret;

	yz_host_lsm_init();

	yz_zygote_probe_init();
	yz_zygote_nl_init();
	yz_zygote_orch_init();
	yz_zygote_ctl_init();

	ret = yukizygisk_control_init();
	if (ret) {
		pr_err("yukizygisk: control device init failed: %d\n", ret);
		goto err_control;
	}

	if (yz_enable) {
		ret = yz_host_set_feature(YZ_FEATURE_YUKIZYGISK, 1);
		if (ret)
			pr_warn("yukizygisk: initial enable failed: %d\n", ret);
	}

	pr_info("yukizygisk: standalone LKM initialized\n");
	return 0;

err_control:
	yz_zygote_ctl_exit();
	yz_zygote_orch_exit();
	yz_zygote_nl_exit();
	yz_zygote_probe_exit();
	yz_host_lsm_exit();
	yz_host_exit();
	return ret;
}

static void __exit yukizygisk_exit(void)
{
	pr_info("yukizygisk: standalone LKM exiting\n");

	yukizygisk_control_exit();
	yz_zygote_ctl_exit();
	yz_zygote_orch_exit();
	yz_zygote_nl_exit();
	yz_zygote_probe_exit();
	yz_host_lsm_exit();
	yz_host_exit();
}

module_init(yukizygisk_init);
module_exit(yukizygisk_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Anatdx");
MODULE_DESCRIPTION("Standalone YukiZygisk kernel LKM");
