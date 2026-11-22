// SPDX-License-Identifier: GPL-2.0

#include <linux/mdio.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/phy.h>

#define AR8XXX_PHY_PAGE        0x18
#define AR8XXX_PHY_DATA_BASE   0x10
#define AR8XXX_REG_CTRL        0x0000
#define AR8XXX_REG_POSTRIP     0x0008
#define AR8XXX_CTRL_VERSION    GENMASK(15, 8)
#define AR8316_VERSION        0x1000
#define AR8316E_VERSION       0x1100

/* OpenWrt ar8216.h definitions, mirrored here to keep the probe standalone. */
#define AR8316_POSTRIP_MAC0_GMII_EN   BIT(0)
#define AR8316_POSTRIP_MAC0_RGMII_EN  BIT(1)
#define AR8316_POSTRIP_PHY4_GMII_EN   BIT(2)
#define AR8316_POSTRIP_PHY4_RGMII_EN  BIT(3)

#define AR8316_PORT_BASE(p)		(0x100 * ((p) + 1))
#define AR8316_PORT_COUNT		6

static int ar8316_read32(struct mii_bus *bus, u32 reg, u32 *value)
{
	u16 page = reg >> 9;
	u16 phy = AR8XXX_PHY_DATA_BASE | ((reg >> 6) & 0x7);
	u16 mii = (reg >> 1) & 0x1e;
	int lo, hi, ret;

	if (!value)
		return -EINVAL;

	mutex_lock(&bus->mdio_lock);

	ret = __mdiobus_write(bus, AR8XXX_PHY_PAGE, 0, page);
	if (ret)
		goto out;

	usleep_range(1000, 2000);
	lo = __mdiobus_read(bus, phy, mii);
	if (lo < 0) {
		ret = lo;
		goto out;
	}

	hi = __mdiobus_read(bus, phy, mii + 1);
	if (hi < 0) {
		ret = hi;
		goto out;
	}

	*value = ((u32)(hi & 0xffff) << 16) | (u32)(lo & 0xffff);
	ret = 0;

out:
	mutex_unlock(&bus->mdio_lock);
	return ret;
}

static int ar8316_write32(struct mii_bus *bus, u32 reg, u32 value)
{
	u16 phy = AR8XXX_PHY_DATA_BASE | ((reg >> 6) & 7);
	u16 mii = (reg >> 1) & 0x1e;
	int ret;

	mutex_lock(&bus->mdio_lock);
	ret = __mdiobus_write(bus, AR8XXX_PHY_PAGE, 0, reg >> 9);
	if (!ret) {
		usleep_range(1000, 2000);
		ret = __mdiobus_write(bus, phy, mii + 1, value >> 16);
	}
	if (!ret)
		ret = __mdiobus_write(bus, phy, mii, value & 0xffff);
	mutex_unlock(&bus->mdio_lock);
	return ret;
}

/* hw_ar8316_lsw_led_init on HG8245V2. Enable the LED pins after
 * programming all three rule registers. Keep the negotiated MAC/PHY mode.
 */
static int ar8316_front_panel(struct mii_bus *bus, u32 postrip)
{
	u32 reg, readback;
	int ret;

	for (reg = 0xb0; reg <= 0xb8; reg += 4) {
		ret = ar8316_write32(bus, reg, 0xcf35cf35);
		if (!ret)
			ret = ar8316_read32(bus, reg, &readback);
		if (ret || readback != 0xcf35cf35)
			return ret ?: -EIO;
	}
	postrip = (postrip | BIT(31)) & ~GENMASK(25, 24);
	ret = ar8316_write32(bus, AR8XXX_REG_POSTRIP, postrip);
	if (!ret)
		ret = ar8316_read32(bus, AR8XXX_REG_POSTRIP, &readback);
	if (ret)
		return ret;
	return ((postrip ^ readback) & (BIT(31) | GENMASK(25, 24))) ? -EIO : 0;
}

static int ar8316_sd5113_probe(struct mdio_device *mdiodev)
{
	struct mii_bus *bus = mdiodev->bus;
	u32 reg0, postrip;
	u32 discriminator;
	int ret;

	ret = ar8316_read32(bus, AR8XXX_REG_CTRL, &reg0);
	if (ret)
		return dev_err_probe(&mdiodev->dev, ret,
				     "AR8316 reg0 read failed\n");

	discriminator = reg0 & AR8XXX_CTRL_VERSION;
	if (discriminator != AR8316_VERSION && discriminator != AR8316E_VERSION)
		return dev_err_probe(&mdiodev->dev, -ENODEV,
				     "unexpected switch ID register: %#08x\n", reg0);

	ret = ar8316_read32(bus, AR8XXX_REG_POSTRIP, &postrip);
	if (ret)
		return dev_err_probe(&mdiodev->dev, ret,
				     "AR8316 POSTRIP(0x08) read failed\n");

	dev_info(&mdiodev->dev,
		 "%s probe: reg0=0x%08x POSTRIP=0x%08x\n",
		 discriminator == AR8316_VERSION ? "AR8316" : "AR8316E",
		 reg0, postrip);

	dev_info(&mdiodev->dev,
		 "POSTRIP decoded: MAC0_GMII=%u MAC0_RGMII=%u PHY4_GMII=%u PHY4_RGMII=%u\n",
		 !!(postrip & AR8316_POSTRIP_MAC0_GMII_EN),
		 !!(postrip & AR8316_POSTRIP_MAC0_RGMII_EN),
		 !!(postrip & AR8316_POSTRIP_PHY4_GMII_EN),
		 !!(postrip & AR8316_POSTRIP_PHY4_RGMII_EN));

	if (of_property_read_bool(mdiodev->dev.of_node, "hisilicon,trace-ports")) {
		unsigned int port;

		for (port = 0; port < AR8316_PORT_COUNT; port++) {
			u32 status, control, vlan;
			u32 base = AR8316_PORT_BASE(port);

			ret = ar8316_read32(bus, base, &status);
			if (!ret)
				ret = ar8316_read32(bus, base + 4, &control);
			if (!ret)
				ret = ar8316_read32(bus, base + 8, &vlan);
			if (ret) {
				dev_warn(&mdiodev->dev, "could not read port %u: %d\n",
					 port, ret);
				break;
			}
			dev_info(&mdiodev->dev,
				 "port %u: status=%08x control=%08x vlan=%08x\n",
				 port, status, control, vlan);
		}
	}

	if (discriminator == AR8316_VERSION &&
	    of_property_read_bool(mdiodev->dev.of_node, "huawei,front-panel-leds")) {
		ret = ar8316_front_panel(bus, postrip);
		if (ret)
			return dev_err_probe(&mdiodev->dev, ret, "LAN LED setup failed\n");
		dev_info(&mdiodev->dev, "LAN1-LAN4 hardware LED rules enabled\n");
	}
	return 0;
}

static const struct of_device_id ar8316_sd5113_of_match[] = {
	{ .compatible = "hisilicon,sd5113-ar8316-probe" },
	{ }
};
MODULE_DEVICE_TABLE(of, ar8316_sd5113_of_match);

static struct mdio_driver ar8316_sd5113_driver = {
	.mdiodrv.driver = {
		.name = "ar8316-sd5113-probe",
		.of_match_table = ar8316_sd5113_of_match,
	},
	.probe = ar8316_sd5113_probe,
};

mdio_module_driver(ar8316_sd5113_driver);

MODULE_DESCRIPTION("AR8316 identification and HG8245V2 front panel LEDs");
MODULE_LICENSE("GPL");
