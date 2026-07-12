# SPDX-License-Identifier: Apache-2.0 OR GPL-2.0
# YukiZygisk integrated (built-in) object list, consumed ONLY by the main repo
# (ReSukiSU/SukiSU) via: include $(src)/yukizygisk/Kbuild.inc
#
# This uses the integrated entry (core/module_integrated.o) and EXCLUDES the
# LKM entry (core/module.o) and bootstrap (core/bootstrap.o). The LKM tree
# stays intact in the same directory for the fork's own DDK .ko build; those
# files simply are not listed here, so they are never compiled into the kernel.

ifdef CONFIG_KSU_YUKIZYGISK

kernelsu-objs += yukizygisk/core/module_integrated.o
kernelsu-objs += yukizygisk/core/control.o
kernelsu-objs += yukizygisk/host/runtime.o
kernelsu-objs += yukizygisk/host/root_impl.o
kernelsu-objs += yukizygisk/host/mount.o
kernelsu-objs += yukizygisk/host/policy.o
kernelsu-objs += yukizygisk/host/policy_temp.o
kernelsu-objs += yukizygisk/host/patch_text.o
kernelsu-objs += yukizygisk/host/lsm.o
kernelsu-objs += yukizygisk/host/adapter.o
kernelsu-objs += yukizygisk/feature/zygote_probe.o
kernelsu-objs += yukizygisk/feature/zygote_orch.o
kernelsu-objs += yukizygisk/feature/zygote_nl.o
kernelsu-objs += yukizygisk/feature/zygote_ctl.o

endif # CONFIG_KSU_YUKIZYGISK