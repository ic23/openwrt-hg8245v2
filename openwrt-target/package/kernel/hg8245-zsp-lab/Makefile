include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=hg8245-zsp-lab
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKG_BUILD_DIR:=$(KERNEL_BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define KernelPackage/hg8245-zsp-lab
  SUBMENU:=Other modules
  TITLE:=HG8245 voice DSP one-shot boot
  DEPENDS:=@TARGET_sd5113
  FILES:=$(PKG_BUILD_DIR)/hg8245_zsp_stage.ko
endef

define Build/Prepare
	$(INSTALL_DIR) $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	+$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef

define KernelPackage/hg8245-zsp-lab/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/lib/firmware/hg8245
	$(INSTALL_BIN) ./files/hg8245-zsp-lab $(1)/usr/sbin/hg8245-zsp-lab
	$(INSTALL_DATA) ./firmware/zsp_min.out $(1)/lib/firmware/hg8245/zsp_min.out
endef

$(eval $(call KernelPackage,hg8245-zsp-lab))
