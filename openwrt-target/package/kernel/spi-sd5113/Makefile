include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=spi-sd5113
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define KernelPackage/spi-sd5113
  SUBMENU:=SPI Support
  TITLE:=HiSilicon SD5113 SPI master
  DEPENDS:=@TARGET_sd5113
  KCONFIG:=CONFIG_SPI=y \
           CONFIG_SPI_MASTER=y \
           CONFIG_SPI_SD5113
  FILES:=$(LINUX_DIR)/drivers/spi/spi-sd5113.ko
endef

define KernelPackage/spi-sd5113/description
 Kernel driver for the SD5113 SPI controller. This package does not
 configure module autoload or supply SPI client drivers.
endef

# Kbuild compiles the module in-tree; there is no external Module.symvers.
define KernelPackage/hooks
endef

define Build/Compile
	grep -Fx 'CONFIG_SPI_SD5113=m' "$(LINUX_DIR)/.config"
endef

$(eval $(call KernelPackage,spi-sd5113))
