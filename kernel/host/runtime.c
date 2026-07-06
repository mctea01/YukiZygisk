/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0 */
/*
 * YukiZygisk - Host runtime symbol resolver.
 *
 * License: Author's work under Apache-2.0; when used as a kernel module
 * (or linked with the Linux kernel), GPL-2.0 applies for kernel compatibility.
 *
 * Author: Anatdx
 */

#include <linux/err.h>
#include <linux/kprobes.h>
#include <linux/moduleparam.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/version.h>

#include "host/root_impl.h"
#include "host/runtime.h"

static bool yz_skip_kallsyms;
module_param_named(skip_kallsyms, yz_skip_kallsyms, bool, 0600);
MODULE_PARM_DESC(skip_kallsyms,
		 "Skip kallsyms_lookup_name bootstrap and use per-symbol kprobe resolution.");

unsigned long (*yz_kallsyms_lookup_name)(const char *name);
struct file *(*yz_filp_open)(const char *filename, int flags, umode_t mode);
int (*yz_filp_close)(struct file *file, fl_owner_t id);

static long (*yz_copy_from_kernel_nofault_fn)(void *dst, const void *src,
					      size_t size);
static bool yz_copy_from_kernel_nofault_tried;
static bool (*yz_kallsyms_lookup_size_offset_fn)(unsigned long addr,
						 unsigned long *symbolsize,
						 unsigned long *offset);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
#define YZ_USE_KCFI 1
#else
#define YZ_USE_KCFI 0
#endif

bool yz_valid_kernel_addr(unsigned long addr)
{
	if (!addr)
		return false;
	if (IS_ERR_VALUE(addr))
		return false;
#if defined(CONFIG_64BIT)
	return (addr & (1UL << 63)) != 0;
#else
	return addr >= PAGE_OFFSET;
#endif
}

YZ_NOCFI unsigned long yz_lookup_name(const char *name)
{
	if (yz_kallsyms_lookup_name) {
		unsigned long addr = yz_kallsyms_lookup_name(name);

		if (addr && !IS_ERR_VALUE(addr))
			return addr;
	}

	{
		struct kprobe kp = { .symbol_name = name };
		unsigned long addr;
		int ret;

		ret = register_kprobe(&kp);
		if (ret < 0) {
			pr_alert("yukizygisk: kprobe %s failed: %d\n", name,
				 ret);
			return 0;
		}
		addr = (unsigned long)kp.addr;
		unregister_kprobe(&kp);
		if (!addr || IS_ERR_VALUE(addr)) {
			pr_alert("yukizygisk: symbol %s invalid addr 0x%lx\n",
				 name, addr);
			return 0;
		}
		return addr;
	}
}

YZ_NOCFI unsigned long yz_lookup_name_quiet(const char *name)
{
	if (yz_kallsyms_lookup_name) {
		unsigned long addr = yz_kallsyms_lookup_name(name);

		if (addr && !IS_ERR_VALUE(addr))
			return addr;
	}

	{
		struct kprobe kp = { .symbol_name = name };
		unsigned long addr;
		int ret;

		ret = register_kprobe(&kp);
		if (ret < 0)
			return 0;
		addr = (unsigned long)kp.addr;
		unregister_kprobe(&kp);
		if (!addr || IS_ERR_VALUE(addr))
			return 0;
		return addr;
	}
}

YZ_NOCFI unsigned long yz_lookup_callable(const char *name)
{
	unsigned long addr;

#if YZ_USE_KCFI
	addr = yz_lookup_name(name);
	if (addr)
		return addr;
#endif
	if (yz_kallsyms_lookup_name) {
		char jt[256];

		if (snprintf(jt, sizeof(jt), "%s.cfi_jt", name) <
		    (int)sizeof(jt)) {
			addr = yz_kallsyms_lookup_name(jt);
			if (addr && !IS_ERR_VALUE(addr))
				return addr;
			}
	}
#if YZ_USE_KCFI
	return 0;
#else
	return yz_lookup_name(name);
#endif
}

YZ_NOCFI unsigned long yz_lookup_callable_quiet(const char *name)
{
	unsigned long addr;

#if YZ_USE_KCFI
	addr = yz_lookup_name_quiet(name);
	if (addr)
		return addr;
#endif
	if (yz_kallsyms_lookup_name) {
		char jt[256];

		if (snprintf(jt, sizeof(jt), "%s.cfi_jt", name) <
		    (int)sizeof(jt)) {
			addr = yz_kallsyms_lookup_name(jt);
			if (addr && !IS_ERR_VALUE(addr))
				return addr;
			}
	}
#if YZ_USE_KCFI
	return 0;
#else
	return yz_lookup_name_quiet(name);
#endif
}

static void yz_resolve_kallsyms_lookup(void)
{
	struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };
	int ret;

	pr_info("yukizygisk: resolving kallsyms_lookup_name\n");
	ret = register_kprobe(&kp);
	if (ret < 0) {
		pr_warn("yukizygisk: kprobe kallsyms_lookup_name failed: %d, using per-symbol kprobe\n",
			ret);
		return;
	}
	if (!yz_valid_kernel_addr((unsigned long)kp.addr)) {
		pr_warn("yukizygisk: invalid kallsyms_lookup_name addr 0x%lx\n",
			(unsigned long)kp.addr);
		unregister_kprobe(&kp);
		return;
	}
	yz_kallsyms_lookup_name = (void *)kp.addr;
	unregister_kprobe(&kp);
	pr_info("yukizygisk: kallsyms_lookup_name resolved @ 0x%lx\n",
		(unsigned long)yz_kallsyms_lookup_name);
}

bool yz_kernel_read_nofault(void *dst, unsigned long src, size_t size)
{
	if (!yz_copy_from_kernel_nofault_tried) {
		unsigned long addr =
			yz_lookup_callable_quiet("copy_from_kernel_nofault");

		yz_copy_from_kernel_nofault_tried = true;
		if (addr && yz_valid_kernel_addr(addr))
			yz_copy_from_kernel_nofault_fn = (void *)addr;
	}

	return yz_copy_from_kernel_nofault_fn &&
	       yz_copy_from_kernel_nofault_fn(dst, (const void *)src, size) == 0;
}

bool yz_lookup_size_offset(unsigned long addr, unsigned long *symbolsize,
			   unsigned long *offset)
{
	if (!yz_kallsyms_lookup_size_offset_fn) {
		unsigned long sym =
			yz_lookup_callable_quiet("kallsyms_lookup_size_offset");

		if (sym && yz_valid_kernel_addr(sym))
			yz_kallsyms_lookup_size_offset_fn = (void *)sym;
	}

	return yz_kallsyms_lookup_size_offset_fn &&
	       yz_kallsyms_lookup_size_offset_fn(addr, symbolsize, offset);
}

static int yz_resolve_runtime_symbols(void)
{
	yz_filp_open = (void *)yz_lookup_callable_quiet("filp_open");
	yz_filp_close = (void *)yz_lookup_callable_quiet("filp_close");

	if (!yz_filp_open)
		pr_warn("yukizygisk: filp_open not found, path-based root detection is limited\n");
	if (!yz_filp_close)
		pr_warn("yukizygisk: filp_close not found, falling back to fput\n");

	return 0;
}

int yz_host_runtime_init(void)
{
	if (!yz_skip_kallsyms)
		yz_resolve_kallsyms_lookup();
	else
		pr_info("yukizygisk: skipping kallsyms bootstrap\n");

	yz_host_root_detect();
	return yz_resolve_runtime_symbols();
}

void yz_host_runtime_exit(void)
{
	yz_filp_open = NULL;
	yz_filp_close = NULL;
	yz_kallsyms_lookup_name = NULL;
	yz_kallsyms_lookup_size_offset_fn = NULL;
	yz_copy_from_kernel_nofault_fn = NULL;
	yz_copy_from_kernel_nofault_tried = false;
}
