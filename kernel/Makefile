# SPDX-License-Identifier: Apache-2.0 OR GPL-2.0
#
# YukiZygisk - External-module build wrapper for the kernel directory.
#
# License: Author's work under Apache-2.0; when used as a kernel module
# (or linked with the Linux kernel), GPL-2.0 applies for kernel compatibility.
#
# Author: Anatdx

ifneq ($(KERNELRELEASE),)
include $(src)/Kbuild
else

KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all modules clean

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

endif
