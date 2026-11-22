include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=le88266
PKG_VERSION:=2.24.0
PKG_RELEASE:=10
VP_REV:=a49d39ee17c6b083a48ed583c3644f154127b593
PKG_SOURCE:=$(VP_REV).tar.gz
PKG_SOURCE_URL:=https://github.com/pgid69/bcm63xx-phone/archive/
PKG_HASH:=703115607717a6d314c3ac949d722b7e603004c6cfaa210c501e19bbd6a6d3c1
PKG_BUILD_DIR:=$(KERNEL_BUILD_DIR)/$(PKG_NAME)
include $(INCLUDE_DIR)/package.mk

define KernelPackage/le88266
  SUBMENU:=Other modules
  TITLE:=Le88266 dual FXS line controller
  DEPENDS:=@TARGET_sd5113 +kmod-spi-sd5113 +kmod-sound-soc-core
  FILES:=$(PKG_BUILD_DIR)/le88266.ko
  AUTOLOAD:=$(call AutoLoad,65,spi-sd5113 le88266)
endef

define Build/Prepare
	$(INSTALL_DIR) $(PKG_BUILD_DIR)
	python3 ./prepare.py $(DL_DIR)/$(PKG_SOURCE) $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef
define Build/Compile
	+$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef
define KernelPackage/le88266/install
	$(INSTALL_DIR) $(1)/lib/firmware/hg8245
	$(INSTALL_DATA) ./firmware/zsp_min.out $(1)/lib/firmware/hg8245/
endef
$(eval $(call KernelPackage,le88266))
