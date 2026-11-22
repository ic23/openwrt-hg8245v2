include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=sd5113-gpon-rx
PKG_RELEASE:=10
PKG_LICENSE:=GPL-2.0-only
PKG_BUILD_DIR:=$(KERNEL_BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define KernelPackage/sd5113-gpon-rx
  SUBMENU:=Network Devices
  TITLE:=SD5113 GPON receiver status
  DEPENDS:=@TARGET_sd5113
  AUTOLOAD:=$(if $(filter y,$(CONFIG_PACKAGE_kmod-hg8245-zsp-lab)),,$(call AutoLoad,66,sd5113_gpon_rx))
  FILES:=$(PKG_BUILD_DIR)/sd5113_gpon_rx.ko
endef

define Build/Prepare
	$(INSTALL_DIR) $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	+$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef

$(eval $(call KernelPackage,sd5113-gpon-rx))
