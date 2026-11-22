include $(TOPDIR)/rules.mk

PKG_NAME:=sd5113-omcid
PKG_VERSION:=1
PKG_RELEASE:=3
PKG_LICENSE:=GPL-2.0-only
include $(INCLUDE_DIR)/package.mk

define Package/sd5113-omcid
  SECTION:=net
  CATEGORY:=Network
  TITLE:=SD5113 ONU OMCI agent
  DEPENDS:=@TARGET_sd5113 +kmod-sd5113-gpon-rx
endef

define Build/Prepare
	$(INSTALL_DIR) $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) -std=gnu11 -Wall -o $(PKG_BUILD_DIR)/sd5113-omcid $(PKG_BUILD_DIR)/*.c $(TARGET_LDFLAGS)
endef

define Package/sd5113-omcid/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d $(1)/usr/share/sd5113-omcid
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/sd5113-omcid $(1)/usr/sbin/
	$(INSTALL_BIN) ./files/omcid.init $(1)/etc/init.d/sd5113-omcid
	$(INSTALL_DATA) ./files/board.mib $(1)/usr/share/sd5113-omcid/
endef

$(eval $(call BuildPackage,sd5113-omcid))
