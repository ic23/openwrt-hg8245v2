include $(TOPDIR)/rules.mk

LUCI_TITLE:=GPON management for the HG8245
LUCI_DEPENDS:=@TARGET_sd5113 +luci-base +kmod-sd5113-gpon-rx +kmod-hpsf2120
PKG_VERSION:=1.0
PKG_RELEASE:=4
PKG_LICENSE:=GPL-2.0-only
include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
