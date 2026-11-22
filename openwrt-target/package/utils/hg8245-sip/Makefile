include $(TOPDIR)/rules.mk

PKG_NAME:=hg8245-sip
PKG_VERSION:=2.15.1
PKG_RELEASE:=1
PKG_SOURCE:=pjproject-$(PKG_VERSION).tar.gz
PKG_SOURCE_URL:=https://codeload.github.com/pjsip/pjproject/tar.gz/refs/tags/
PKG_SOURCE_URL_FILE:=$(PKG_VERSION)
PKG_HASH:=8f3bd99caf003f96ed8038b8a36031eb9d8cd9eaea1eaff7e01c2eef6bd55706
PKG_BUILD_DIR:=$(BUILD_DIR)/pjproject-$(PKG_VERSION)
PKG_LICENSE:=GPL-2.0-or-later
PKG_BUILD_PARALLEL:=1
include $(INCLUDE_DIR)/package.mk

define Package/hg8245-sip
  SECTION:=net
  CATEGORY:=Network
  TITLE:=HG8245V2 Ethernet SIP handset
  DEPENDS:=@TARGET_sd5113 +kmod-le88266 +libpthread +librt
endef

CONFIGURE_ARGS += --disable-video --disable-pjsua2 --disable-sound --disable-resample \
	--disable-gsm-codec --disable-l16-codec --disable-g722-codec --disable-g7221-codec \
	--disable-speex-codec --disable-ilbc-codec --disable-speex-aec --disable-floating-point \
	--disable-ssl --disable-libuuid --disable-upnp --disable-opus --disable-libwebrtc \
	--disable-libyuv --disable-ffmpeg --disable-v4l2 --disable-opencore-amr --disable-libsrtp
TARGET_CFLAGS += -ffunction-sections -fdata-sections
define Build/Prepare
	$(call Build/Prepare/Default)
	$(CP) ./src/handset.c ./src/handset.mk $(PKG_BUILD_DIR)/
	$(CP) ./src/config_site.h $(PKG_BUILD_DIR)/pjlib/include/pj/
endef
define Build/Compile
	+$(MAKE) $(PKG_JOBS) -C $(PKG_BUILD_DIR) lib
	$(MAKE) -C $(PKG_BUILD_DIR) -f handset.mk
endef
define Package/hg8245-sip/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d $(1)/etc/config
	$(INSTALL_BIN) ./files/hg8245-sip.init $(1)/etc/init.d/hg8245-sip
	$(INSTALL_CONF) ./files/hg8245.config $(1)/etc/config/hg8245
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/hg8245-sip $(1)/usr/sbin/
endef
$(eval $(call BuildPackage,hg8245-sip))
