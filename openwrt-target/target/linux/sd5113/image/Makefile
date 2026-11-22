# SPDX-License-Identifier: GPL-2.0-only

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/image.mk

HG8245_DTS := hisilicon-sd5113-hg8245
ifeq ($(CONFIG_PACKAGE_kmod-hg8245-le88226-id),y)
  HG8245_DTS := hisilicon-sd5113-hg8245-le88226-id
  ifeq ($(CONFIG_PACKAGE_kmod-hg8245-zsp-lab),y)
    HG8245_DTS := hisilicon-sd5113-hg8245-voice-chain
  endif
else ifeq ($(CONFIG_PACKAGE_kmod-spi-sd5113),y)
  HG8245_DTS := hisilicon-sd5113-hg8245-spi
endif
ifeq ($(CONFIG_PACKAGE_kmod-input-gpio-keys-polled),y)
  HG8245_DTS := hisilicon-sd5113-hg8245-board-io
endif
ifeq ($(CONFIG_PACKAGE_kmod-sd5113-gpon-rx),y)
  HG8245_DTS := hisilicon-sd5113-hg8245-gpon-rx
endif

# The board contains both peripherals even when only one driver is installed.
ifneq ($(CONFIG_PACKAGE_kmod-hg8245-zsp-lab),y)
  ifneq ($(filter y,$(CONFIG_PACKAGE_kmod-le88266) $(CONFIG_PACKAGE_kmod-sd5113-gpon-rx)),)
    HG8245_DTS := hisilicon-sd5113-hg8245-fxs
    ifeq ($(CONFIG_PACKAGE_hg8245-leds),y)
      HG8245_DTS := hisilicon-sd5113-hg8245-panel
      ifeq ($(CONFIG_PACKAGE_hostapd-mbedtls)$(CONFIG_PACKAGE_kmod-gpio-button-hotplug),yy)
        HG8245_DTS := hisilicon-sd5113-hg8245-panel-wifi
      endif
    endif
  endif
endif

define Device/sd5113_hg8245
  DEVICE_VENDOR := Huawei
  DEVICE_MODEL := HG8245
  SOC := sd5113
  DEVICE_DTS := $(HG8245_DTS)
  DEVICE_DTS_DIR := $(DTS_DIR)/hisilicon
  KERNEL_NAME := zImage
  KERNEL_LOADADDR := 0x91000000
  KERNEL := kernel-bin | append-dtb | uImage none
  KERNEL_INITRAMFS := $$(KERNEL) | check-size 16m
endef
TARGET_DEVICES += sd5113_hg8245

$(eval $(call BuildImage))
