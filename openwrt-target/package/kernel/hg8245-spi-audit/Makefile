include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=hg8245-spi-audit
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKG_BUILD_DIR:=$(KERNEL_BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define KernelPackage/hg8245-spi-audit
  SUBMENU:=Other modules
  TITLE:=HG8245 UART SPI diagnostics
  DEPENDS:=@TARGET_sd5113
  FILES:=$(PKG_BUILD_DIR)/hg8245_slicprobe.ko
endef

define Build/Prepare
	$(INSTALL_DIR) $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	+$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" \
		KCFLAGS="$(KERNEL_CFLAGS) -DHG8245_SLICPROBE_UART -DHG8245_SLICPROBE_CTRL_TEST" modules
endef

define KernelPackage/hg8245-spi-audit/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d
	$(INSTALL_BIN) ./files/hg8245-spi-audit $(1)/usr/sbin/hg8245-spi-audit
	$(INSTALL_BIN) ./files/hg8245-spi-audit.init $(1)/etc/init.d/hg8245-spi-audit
endef

$(eval $(call KernelPackage,hg8245-spi-audit))
