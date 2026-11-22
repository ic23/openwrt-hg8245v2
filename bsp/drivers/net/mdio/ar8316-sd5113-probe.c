// SPDX-License-Identifier: GPL-2.0

#include <linux/mdio.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/phy.h>

#define AR8XXX_PHY_PAGE        0x18
#define AR8XXX_PHY_DATA_BASE   0x10
#define AR8XXX_REG_POSTRIP     0x0008

/* OpenWrt ar8216.h definitions, mirrored here to keep the probe standalone. */
#define AR8316_POSTRIP_MAC0_GMII_EN   BIT(0)
#define AR8316_POSTRIP_MAC0_RGMII_EN  BIT(1)
#define AR8316_POSTRIP_PHY4_GMII_EN   BIT(2)
#define AR8316_POSTRIP_PHY4_RGMII_EN  BIT(3)

static int ar8316_read32(struct mii_bus *bus, u32 reg, u32 *value)
{
	u16 page = reg >> 9;
	u16 phy = AR8XXX_PHY_DATA_BASE | ((reg >> 6) & 0x7);
	u16 mii = (reg >> 1) & 0x1e;
	int lo, hi, ret;

	if (!value)
		return -EINVAL;

	mutex_lock(&bus->mdio_lock);

	ret = bus->write(bus, AR8XXX_PHY_PAGE, 0, page);
	if (ret)
		goto out;

	lo = bus->read(bus, phy, mii);
	if (lo < 0) {
		ret = lo;
		goto out;
	}

	hi = bus->read(bus, phy, mii + 1);
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

static int ar8316_sd5113_probe(struct mdio_device *mdiodev)
{
	struct mii_bus *bus = mdiodev->bus;
	u32 reg0, postrip;
	int ret;
	u32 discriminator;

	ret = ar8316_read32(bus, 0x0000, &reg0);
	if (ret)
		return dev_err_probe(&mdiodev->dev, ret,
				     "AR8316 reg0 read failed\n");

	ret = ar8316_read32(bus, AR8XXX_REG_POSTRIP, &postrip);
	if (ret)
		return dev_err_probe(&mdiodev->dev, ret,
				     "AR8316 POSTRIP(0x08) read failed\n");

	discriminator = reg0 & 0x0000ff00;

	dev_info(&mdiodev->dev,
		 "AR8316 probe: reg0=0x%08x discriminator=0x%04x POSTRIP=0x%08x\n",
		 reg0, discriminator >> 8, postrip);

	if (discriminator == 0x1000)
		dev_info(&mdiodev->dev, "vendor discriminator: AR8316 path\n");
	else if (discriminator == 0x1100)
		dev_warn(&mdiodev->dev,
			 "vendor discriminator: AR8316E path (unexpected for this log)\n");
	else
		dev_warn(&mdiodev->dev,
			 "vendor discriminator: unknown value 0x%04x\n",
			 discriminator >> 8);

	dev_info(&mdiodev->dev,
		 "POSTRIP decoded: MAC0_GMII=%u MAC0_RGMII=%u PHY4_GMII=%u PHY4_RGMII=%u\n",
		 !!(postrip & AR8316_POSTRIP_MAC0_GMII_EN),
		 !!(postrip & AR8316_POSTRIP_MAC0_RGMII_EN),
		 !!(postrip & AR8316_POSTRIP_PHY4_GMII_EN),
		 !!(postrip & AR8316_POSTRIP_PHY4_RGMII_EN));

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

MODULE_DESCRIPTION("Read-only AR8316 identification probe for SD5113 MDIO");
MODULE_LICENSE("GPL");
