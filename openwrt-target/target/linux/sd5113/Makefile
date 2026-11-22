# Copyright (C) 2026 OpenWrt.org
# This is released under the terms of the GNU GPL v2.0

include $(TOPDIR)/rules.mk

ARCH:=arm
BOARD:=sd5113
BOARDNAME:=HiSilicon SD5113 (Huawei HG8245 ONT/router)
FEATURES:=ramdisk pci usb dt audio
CPU_TYPE:=arm1176jzf-s
SUBTARGETS:=generic

KERNEL_PATCHVER:=6.6

define Target/Description
	Build a RAM-only bring-up image for the Huawei HG8245.
endef

KERNELNAME:=zImage dtbs

include $(INCLUDE_DIR)/target.mk

$(eval $(call BuildTarget))
