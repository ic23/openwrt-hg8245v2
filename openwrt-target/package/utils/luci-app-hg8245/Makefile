include $(TOPDIR)/rules.mk
PKG_NAME:=luci-app-hg8245
PKG_RELEASE:=2
PKG_LICENSE:=GPL-2.0-only
include $(INCLUDE_DIR)/package.mk

define Package/luci-app-hg8245
  SECTION:=luci
  CATEGORY:=LuCI
  TITLE:=HG8245 management (transitional package)
  DEPENDS:=+luci-app-hg8245-telephony +luci-app-sd5113-gpon +hg8245-rpc-compat
  PKGARCH:=all
endef

define Package/hg8245-rpc-compat
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Compatibility for the original HG8245 ubus commands
  DEPENDS:=+rpcd +ucode +ucode-mod-ubus
  PKGARCH:=all
endef

define Build/Compile
endef

define Package/hg8245-rpc-compat/install
	$(INSTALL_DIR) $(1)/usr/libexec/rpcd
	$(INSTALL_BIN) ./files/usr/libexec/rpcd/hg8245 $(1)/usr/libexec/rpcd/
endef

define Package/luci-app-hg8245/install
	true
endef

$(eval $(call BuildPackage,hg8245-rpc-compat))
$(eval $(call BuildPackage,luci-app-hg8245))
