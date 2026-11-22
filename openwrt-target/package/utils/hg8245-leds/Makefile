include $(TOPDIR)/rules.mk

PKG_NAME:=hg8245-leds
PKG_RELEASE:=2
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/hg8245-leds
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=HG8245V2 front panel indicators
  DEPENDS:=@TARGET_sd5113 +kmod-leds-gpio +kmod-ledtrig-pattern
  PKGARCH:=all
endef

define Build/Compile
endef

define Package/hg8245-leds/install
	$(INSTALL_DIR) $(1)/etc/init.d $(1)/lib/hg8245
	$(INSTALL_BIN) ./files/init $(1)/etc/init.d/hg8245-leds
	$(INSTALL_BIN) ./files/front-panel $(1)/lib/hg8245/front-panel
	$(INSTALL_DIR) $(1)/usr/libexec/led-trigger $(1)/etc/hotplug.d/net
	$(INSTALL_DATA) ./files/phy-led $(1)/etc/hotplug.d/net/92-hg8245-phy-led
	$(INSTALL_DATA) ./files/activate $(1)/lib/hg8245/led-activate
	$(foreach event,tel1 tel2 los pon usb wps bbu,$(LN) /lib/hg8245/led-activate $(1)/usr/libexec/led-trigger/hg8245-$(event);)
endef

$(eval $(call BuildPackage,hg8245-leds))
