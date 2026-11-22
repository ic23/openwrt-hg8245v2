include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=hpsf2120
PKG_RELEASE:=2
PKG_LICENSE:=GPL-2.0-only
PKG_BUILD_DIR:=$(KERNEL_BUILD_DIR)/$(PKG_NAME)
include $(INCLUDE_DIR)/package.mk

define KernelPackage/hpsf2120
  SUBMENU:=I2C support
  TITLE:=HPSF2120 optical transceiver EEPROM and diagnostics
  DEPENDS:=@TARGET_sd5113 +kmod-i2c-sd5113 +kmod-hwmon-core
  KCONFIG:=CONFIG_NVMEM=y CONFIG_NVMEM_SYSFS=y
  FILES:=$(PKG_BUILD_DIR)/hpsf2120.ko
  AUTOLOAD:=$(call AutoLoad,60,i2c-sd5113 hpsf2120)
endef

define Build/Prepare
	$(INSTALL_DIR) $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef
define Build/Compile
	+$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef
$(eval $(call KernelPackage,hpsf2120))
