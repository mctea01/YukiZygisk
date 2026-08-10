/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0 */
/*
 * YukiZygisk - prctl bootstrap and root control sessions.
 *
 * License: Author's work under Apache-2.0; when used as a kernel module
 * (or linked with the Linux kernel), GPL-2.0 applies for kernel compatibility.
 *
 * Author: Anatdx
 */

#include <linux/atomic.h>
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/fdtable.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/namei.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/task_work.h>
#include <linux/uaccess.h>
#include <linux/uidgid.h>
#include <linux/workqueue.h>

#include "core/bootstrap.h"
#include "core/control.h"
#include "core/lifecycle.h"
#include "host/runtime.h"
#include "uapi/yukizygisk.h"

enum yz_prctl_abi {
	YZ_PRCTL_ABI_ARM64_WRAPPER,
	YZ_PRCTL_ABI_DIRECT,
};

struct yz_bootstrap_args {
	unsigned long option;
	unsigned long magic1;
	unsigned long cookie_lo;
	unsigned long cookie_hi;
	unsigned long out_fd;
};

struct yz_control_session {
	struct callback_head twork;
	void __user *out_fd;
	pid_t pid;
	bool bootstrap;
};

static unsigned long long yz_bootstrap_cookie_lo;
static unsigned long long yz_bootstrap_cookie_hi;
module_param_named(bootstrap_cookie_lo, yz_bootstrap_cookie_lo, ullong, 0400);
MODULE_PARM_DESC(bootstrap_cookie_lo, "Low 64 bits of the one-shot bootstrap cookie");
module_param_named(bootstrap_cookie_hi, yz_bootstrap_cookie_hi, ullong, 0400);
MODULE_PARM_DESC(bootstrap_cookie_hi, "High 64 bits of the one-shot bootstrap cookie");
static unsigned int yz_bootstrap_guard_delay_sec = 5;
module_param_named(bootstrap_guard_delay_sec, yz_bootstrap_guard_delay_sec,
		   uint, 0644);
MODULE_PARM_DESC(bootstrap_guard_delay_sec, "Seconds between bootstrap guard checks");
static unsigned int yz_bootstrap_guard_max_sec = 30;
module_param_named(bootstrap_guard_max_sec, yz_bootstrap_guard_max_sec, uint,
		   0644);
MODULE_PARM_DESC(bootstrap_guard_max_sec, "Maximum seconds before bootstrap guard fails closed");

enum yz_bootstrap_claim_state {
	YZ_BOOTSTRAP_UNCLAIMED,
	YZ_BOOTSTRAP_CLAIMING,
	YZ_BOOTSTRAP_DELIVERED,
	YZ_BOOTSTRAP_READY,
	YZ_BOOTSTRAP_CLOSED,
};

static atomic_t yz_bootstrap_claim_state =
	ATOMIC_INIT(YZ_BOOTSTRAP_UNCLAIMED);
static struct kprobe yz_bootstrap_kp;
static enum yz_prctl_abi yz_bootstrap_abi;
static bool yz_bootstrap_registered;
static unsigned long yz_bootstrap_guard_deadline;
static DEFINE_MUTEX(yz_bootstrap_kprobe_lock);

static void yz_bootstrap_guard_work_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(yz_bootstrap_guard_work,
			    yz_bootstrap_guard_work_fn);

static void yz_bootstrap_close_fd(unsigned int fd)
{
	yz_close_fd(fd);
}

static void yz_bootstrap_clear_cookie(void)
{
	WRITE_ONCE(yz_bootstrap_cookie_lo, 0);
	WRITE_ONCE(yz_bootstrap_cookie_hi, 0);
}

static void yz_bootstrap_unregister_kprobe(void)
{
	mutex_lock(&yz_bootstrap_kprobe_lock);
	if (yz_bootstrap_registered) {
		unregister_kprobe(&yz_bootstrap_kp);
		yz_bootstrap_registered = false;
		pr_info("yukizygisk: prctl control hook removed\n");
	}
	mutex_unlock(&yz_bootstrap_kprobe_lock);
}

static bool yz_bootstrap_path_exists(const char *path)
{
	struct path p;
	int ret;

	ret = yz_kern_path(path, 0, &p);
	if (ret)
		return false;
	yz_path_put(&p);
	return true;
}

static bool yz_bootstrap_services_or_later(void)
{
	return yz_bootstrap_path_exists("/dev/socket/zygote") ||
	       yz_bootstrap_path_exists("/dev/socket/zygote64");
}

static void yz_bootstrap_guard_work_fn(struct work_struct *work)
{
	unsigned long delay;
	int state;

	(void)work;

	state = atomic_read(&yz_bootstrap_claim_state);
	if (state == YZ_BOOTSTRAP_READY ||
	    state == YZ_BOOTSTRAP_CLOSED)
		return;
	if (state == YZ_BOOTSTRAP_CLAIMING &&
	    time_before(jiffies, yz_bootstrap_guard_deadline)) {
		delay = msecs_to_jiffies(max_t(unsigned int, 1,
					       yz_bootstrap_guard_delay_sec) *
					  1000);
		schedule_delayed_work(&yz_bootstrap_guard_work, delay);
		return;
	}
	if (!yz_bootstrap_services_or_later() &&
	    time_before(jiffies, yz_bootstrap_guard_deadline)) {
		delay = msecs_to_jiffies(max_t(unsigned int, 1,
					       yz_bootstrap_guard_delay_sec) *
					 1000);
		schedule_delayed_work(&yz_bootstrap_guard_work, delay);
		return;
	}

	if (atomic_cmpxchg(&yz_bootstrap_claim_state, state,
			   YZ_BOOTSTRAP_CLOSED) != state) {
		delay = msecs_to_jiffies(max_t(unsigned int, 1,
					       yz_bootstrap_guard_delay_sec) *
					  1000);
		schedule_delayed_work(&yz_bootstrap_guard_work, delay);
		return;
	}

	yz_bootstrap_clear_cookie();
	yz_bootstrap_unregister_kprobe();
	pr_warn("yukizygisk: zygiskd did not become ready before the bootstrap deadline; disabling hooks\n");
	yukizygisk_bootstrap_fail_closed();
	pr_warn("yukizygisk: bootstrap failed closed; waiting for external module unload\n");
}

static void yz_bootstrap_task_work(struct callback_head *head)
{
	struct yz_control_session *session =
		container_of(head, struct yz_control_session, twork);
	int fd;

	fd = yukizygisk_control_install_fd(session->bootstrap);
	if (fd >= 0 && session->bootstrap &&
	    atomic_cmpxchg(&yz_bootstrap_claim_state,
			   YZ_BOOTSTRAP_CLAIMING,
			   YZ_BOOTSTRAP_DELIVERED) !=
		    YZ_BOOTSTRAP_CLAIMING) {
		yz_bootstrap_close_fd((unsigned int)fd);
		fd = -ECANCELED;
	}
	if (fd >= 0 && session->out_fd &&
	    copy_to_user(session->out_fd, &fd, sizeof(fd))) {
		yz_bootstrap_close_fd((unsigned int)fd);
		if (session->bootstrap)
			atomic_cmpxchg(&yz_bootstrap_claim_state,
				       YZ_BOOTSTRAP_DELIVERED,
				       YZ_BOOTSTRAP_UNCLAIMED);
		fd = -EFAULT;
	}

	if (fd < 0)
		pr_warn("yukizygisk: %s fd install failed pid=%d ret=%d\n",
			session->bootstrap ? "bootstrap" : "control",
			session->pid, fd);
	else if (session->bootstrap)
		pr_info("yukizygisk: bootstrap fd delivered pid=%d fd=%d\n",
			session->pid, fd);
	if (session->bootstrap) {
		if (fd < 0) {
			atomic_cmpxchg(&yz_bootstrap_claim_state,
				       YZ_BOOTSTRAP_CLAIMING,
				       YZ_BOOTSTRAP_UNCLAIMED);
		} else {
			yz_bootstrap_clear_cookie();
		}
	}

	module_put(THIS_MODULE);
	kfree(session);
}

static int yz_queue_control_session(void __user *out_fd, bool bootstrap)
{
	struct yz_control_session *session;
	int ret;

	if (!out_fd || !current->mm ||
	    !uid_eq(current_euid(), GLOBAL_ROOT_UID))
		return -EPERM;
	if (!yukizygisk_control_available())
		return -ENODEV;
	if (!try_module_get(THIS_MODULE))
		return -ENODEV;

	session = kzalloc(sizeof(*session), GFP_ATOMIC);
	if (!session) {
		module_put(THIS_MODULE);
		return -ENOMEM;
	}
	session->out_fd = out_fd;
	session->pid = current->pid;
	session->bootstrap = bootstrap;
	init_task_work(&session->twork, yz_bootstrap_task_work);
	ret = yz_task_work_add(current, &session->twork, TWA_RESUME);
	if (ret) {
		kfree(session);
		module_put(THIS_MODULE);
	}
	return ret;
}

int yukizygisk_bootstrap_daemon_ready(void)
{
	int state;

	state = atomic_cmpxchg(&yz_bootstrap_claim_state,
			       YZ_BOOTSTRAP_DELIVERED,
			       YZ_BOOTSTRAP_READY);
	if (state == YZ_BOOTSTRAP_READY)
		return 0;
	if (state != YZ_BOOTSTRAP_DELIVERED)
		return state == YZ_BOOTSTRAP_CLOSED ? -ENODEV : -EAGAIN;

	cancel_delayed_work(&yz_bootstrap_guard_work);
	pr_info("yukizygisk: zygiskd bootstrap ready\n");
	return 0;
}

static bool yz_bootstrap_read_prctl_args(struct pt_regs *regs,
					 struct yz_bootstrap_args *args)
{
#if defined(CONFIG_ARM64)
	struct pt_regs *sysregs;

	if (!regs || !args)
		return false;

	if (yz_bootstrap_abi == YZ_PRCTL_ABI_ARM64_WRAPPER) {
		struct pt_regs syscall_regs;

		sysregs = (struct pt_regs *)regs->regs[0];
		if (sysregs && !copy_from_kernel_nofault(
				       &syscall_regs, sysregs,
				       sizeof(syscall_regs))) {
			args->option = syscall_regs.regs[0];
			args->magic1 = syscall_regs.regs[1];
			args->cookie_lo = syscall_regs.regs[2];
			args->cookie_hi = syscall_regs.regs[3];
			args->out_fd = syscall_regs.regs[4];
			return true;
		}
	}

	args->option = regs->regs[0];
	args->magic1 = regs->regs[1];
	args->cookie_lo = regs->regs[2];
	args->cookie_hi = regs->regs[3];
	args->out_fd = regs->regs[4];
	return true;
#else
	(void)regs;
	(void)args;
	return false;
#endif
}

static int yz_bootstrap_prctl_pre(struct kprobe *kp, struct pt_regs *regs)
{
	struct yz_bootstrap_args args;
	unsigned long long cookie_lo;
	unsigned long long cookie_hi;
	int ret;

	(void)kp;

	if (!yz_bootstrap_read_prctl_args(regs, &args))
		return 0;
	if ((u32)args.option == YZ_PRCTL_CONTROL_OPTION &&
	    (u32)args.magic1 == YZ_PRCTL_CONTROL_MAGIC) {
		(void)yz_queue_control_session((void __user *)args.cookie_lo,
					       false);
		return 0;
	}
	if ((u32)args.option != YZ_PRCTL_BOOTSTRAP_OPTION ||
	    args.magic1 != YZ_PRCTL_BOOTSTRAP_MAGIC_YUKIHOOK)
		return 0;

	cookie_lo = READ_ONCE(yz_bootstrap_cookie_lo);
	cookie_hi = READ_ONCE(yz_bootstrap_cookie_hi);
	if ((!cookie_lo && !cookie_hi) || args.cookie_lo != cookie_lo ||
	    args.cookie_hi != cookie_hi || !args.out_fd)
		return 0;
	if (atomic_cmpxchg(&yz_bootstrap_claim_state,
			   YZ_BOOTSTRAP_UNCLAIMED,
			   YZ_BOOTSTRAP_CLAIMING) !=
	    YZ_BOOTSTRAP_UNCLAIMED)
		return 0;
	ret = yz_queue_control_session((void __user *)args.out_fd, true);
	if (ret) {
		atomic_set(&yz_bootstrap_claim_state,
			   YZ_BOOTSTRAP_UNCLAIMED);
		return 0;
	}

	return 0;
}

struct yz_prctl_candidate {
	const char *symbol;
	enum yz_prctl_abi abi;
};

static int yz_bootstrap_register_prctl_hook(void)
{
#if defined(CONFIG_ARM64)
	static const struct yz_prctl_candidate candidates[] = {
		{ "__se_sys_prctl", YZ_PRCTL_ABI_DIRECT },
		{ "__arm64_sys_prctl", YZ_PRCTL_ABI_ARM64_WRAPPER },
	};
	int ret = -ENOENT;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(candidates); i++) {
		memset(&yz_bootstrap_kp, 0, sizeof(yz_bootstrap_kp));
		yz_bootstrap_kp.symbol_name = candidates[i].symbol;
		yz_bootstrap_kp.pre_handler = yz_bootstrap_prctl_pre;
		yz_bootstrap_abi = candidates[i].abi;
		ret = register_kprobe(&yz_bootstrap_kp);
		if (!ret) {
			yz_bootstrap_registered = true;
			pr_info("yukizygisk: prctl control hook armed on %s\n",
				candidates[i].symbol);
			return 0;
		}
	}

	return ret;
#else
	return -EOPNOTSUPP;
#endif
}

int yukizygisk_bootstrap_init(void)
{
	bool has_cookie;
	int ret;

	has_cookie = READ_ONCE(yz_bootstrap_cookie_lo) ||
		     READ_ONCE(yz_bootstrap_cookie_hi);
	atomic_set(&yz_bootstrap_claim_state,
		   has_cookie ? YZ_BOOTSTRAP_UNCLAIMED : YZ_BOOTSTRAP_CLOSED);

	mutex_lock(&yz_bootstrap_kprobe_lock);
	ret = yz_bootstrap_register_prctl_hook();
	mutex_unlock(&yz_bootstrap_kprobe_lock);
	if (ret) {
		pr_err("yukizygisk: prctl bootstrap hook failed: %d\n", ret);
		return ret;
	}

	if (!has_cookie) {
		pr_warn("yukizygisk: bootstrap cookie missing; daemon bootstrap disabled, root control sessions remain available\n");
	} else {
		unsigned long delay;

		delay = msecs_to_jiffies(max_t(unsigned int, 1,
					       yz_bootstrap_guard_delay_sec) *
					 1000);
		yz_bootstrap_guard_deadline =
			jiffies + msecs_to_jiffies(max_t(unsigned int, 1,
							 yz_bootstrap_guard_max_sec) *
						   1000);
		schedule_delayed_work(&yz_bootstrap_guard_work, delay);
	}
	return ret;
}

void yukizygisk_bootstrap_exit(void)
{
	atomic_set(&yz_bootstrap_claim_state, YZ_BOOTSTRAP_CLOSED);
	yz_bootstrap_clear_cookie();
	cancel_delayed_work_sync(&yz_bootstrap_guard_work);
	yz_bootstrap_unregister_kprobe();
}
