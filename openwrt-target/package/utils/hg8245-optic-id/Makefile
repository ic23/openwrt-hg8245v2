include $(TOPDIR)/rules.mk

PKG_NAME:=hg8245-optic-id
PKG_RELEASE:=4
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/hg8245-optic-id
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=HG8245V2 RAM lab optical identity read
  DEPENDS:=@TARGET_sd5113 +kmod-i2c-sd5113
endef

define Build/Prepare
	$(INSTALL_DIR) $(PKG_BUILD_DIR)
	$(CP) ./src/hg8245-optic-id.c $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) -Wall -Wextra \
		-o $(PKG_BUILD_DIR)/hg8245-optic-id $(PKG_BUILD_DIR)/hg8245-optic-id.c \
	$(TARGET_LDFLAGS) -lm
endef

define Package/hg8245-optic-id/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/hg8245-optic-id $(1)/usr/sbin/
	$(LN) hg8245-optic-id $(1)/usr/sbin/hpsf2120
	$(LN) hg8245-optic-id $(1)/usr/sbin/hg8245-optic
endef

$(eval $(call BuildPackage,hg8245-optic-id))
