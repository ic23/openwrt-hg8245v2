include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=i2c-sd5113
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define KernelPackage/i2c-sd5113
  SUBMENU:=I2C support
  TITLE:=HiSilicon SD5113 hardware I2C controller
  DEPENDS:=@TARGET_sd5113 +kmod-i2c-core
  KCONFIG:=CONFIG_I2C_SD5113
  FILES:=$(LINUX_DIR)/drivers/i2c/busses/i2c-sd5113.ko
endef

define KernelPackage/i2c-sd5113/description
  Manual-load native SD5113 I2C bus driver. It registers an adapter without
  scanning addresses or adding optical EEPROM clients.
endef

define Build/Compile
	grep -Fx 'CONFIG_I2C_SD5113=m' "$(LINUX_DIR)/.config"
endef

$(eval $(call KernelPackage,i2c-sd5113))
