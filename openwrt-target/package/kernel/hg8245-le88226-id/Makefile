include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=hg8245-le88226-id
PKG_RELEASE:=2
PKG_LICENSE:=GPL-2.0-only
PKG_BUILD_DIR:=$(KERNEL_BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define KernelPackage/hg8245-le88226-id
  SUBMENU:=SPI Support
  TITLE:=HG8245 LE88226 SPI identification
  DEPENDS:=@TARGET_sd5113 +kmod-spi-sd5113
  FILES:=$(PKG_BUILD_DIR)/hg8245_le88226_id.ko
endef

define KernelPackage/hg8245-le88226-id/description
  ID-only SPI client for the HG8245 LE88226. This package does not
  configure module autoload.
endef

define Build/Prepare
	$(INSTALL_DIR) $(PKG_BUILD_DIR)
	$(CP) ./src/Makefile ./src/hg8245_le88226_id.c $(PKG_BUILD_DIR)/
endef

define Build/Compile
	+$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef

$(eval $(call KernelPackage,hg8245-le88226-id))
