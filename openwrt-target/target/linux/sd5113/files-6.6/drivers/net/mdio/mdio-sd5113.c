// SPDX-License-Identifier: GPL-2.0
/* HiSilicon SD5113 (Huawei HG8245) hardware MDIO bus controller */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mdio.h>
#include <linux/of_address.h>
#include <linux/of_mdio.h>
#include <linux/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define SD5113_MDIO_CTRL		0x000
#define SD5113_MDIO_CTRL_REGADDR_MASK	GENMASK(4, 0)
#define SD5113_MDIO_CTRL_PHYADDR_SHIFT	8
#define SD5113_MDIO_CTRL_PHYADDR_MASK	GENMASK(12, 8)
#define SD5113_MDIO_CTRL_WRITE		BIT(16)
#define SD5113_MDIO_CTRL_READ		BIT(17)
#define SD5113_MDIO_CTRL_GO		BIT(20)

#define SD5113_MDIO_DATA		0x100
#define SD5113_MDIO_DATA_SHIFT		16

/*
 * Mainline-only timeout policy: 1000 polls x 10us = up to 10ms. The
 * vendor's exact loop count is CONFIRMED as 641, but its per-iteration
 * legacy __const_udelay() duration is not safely convertible here.
 */
#define SD5113_MDIO_POLL_US		10
#define SD5113_MDIO_POLL_RETRIES	1000	/* mainline policy; vendor uses 641 */

/* Vendor enable sequence, see header. Offsets inside the CRG page. */
#define SD5113_MDIO_CRG_GATE_A		0x1c
#define SD5113_MDIO_CRG_GATE_A_BIT	BIT(10)
#define SD5113_MDIO_CRG_GATE_B		0x38
#define SD5113_MDIO_CRG_GATE_B_BIT	BIT(26)
#define SD5113_MDIO_PULSE_REG		0x500
#define SD5113_MDIO_MODE_REG		0x300
#define SD5113_MDIO_PULSE_US		25

struct sd5113_mdio {
	void __iomem *base;
	struct regmap *crg;
};

static int sd5113_mdio_hw_enable(struct sd5113_mdio *mdio)
{
	int ret;
	u32 v;

	ret = regmap_write_bits(mdio->crg, SD5113_MDIO_CRG_GATE_A,
				SD5113_MDIO_CRG_GATE_A_BIT, 0);
	if (ret)
		return ret;

	ret = regmap_write_bits(mdio->crg, SD5113_MDIO_CRG_GATE_B,
				SD5113_MDIO_CRG_GATE_B_BIT, SD5113_MDIO_CRG_GATE_B_BIT);
	if (ret) {
		regmap_set_bits(mdio->crg, SD5113_MDIO_CRG_GATE_A,
				SD5113_MDIO_CRG_GATE_A_BIT);
		return ret;
	}

	v = readl(mdio->base + SD5113_MDIO_PULSE_REG);
	writel(v | 1, mdio->base + SD5113_MDIO_PULSE_REG);
	udelay(SD5113_MDIO_PULSE_US);
	v = readl(mdio->base + SD5113_MDIO_PULSE_REG);
	writel(v & ~1, mdio->base + SD5113_MDIO_PULSE_REG);

	v = readl(mdio->base + SD5113_MDIO_MODE_REG);
	v = ((v & ~3u) | 8u) & ~4u;	/* x = 0 */
	writel(v, mdio->base + SD5113_MDIO_MODE_REG);

	return regmap_write_bits(mdio->crg, SD5113_MDIO_CRG_GATE_A,
				 SD5113_MDIO_CRG_GATE_A_BIT, SD5113_MDIO_CRG_GATE_A_BIT);
}

static int sd5113_mdio_wait_ready(struct sd5113_mdio *mdio)
{
	u32 ctrl;
	int i;

	for (i = 0; i < SD5113_MDIO_POLL_RETRIES; i++) {
		ctrl = readl(mdio->base + SD5113_MDIO_CTRL);
		if (!(ctrl & SD5113_MDIO_CTRL_GO))
			return 0;
		udelay(SD5113_MDIO_POLL_US);
	}

	return -ETIMEDOUT;
}

static u32 sd5113_mdio_build_ctrl(struct sd5113_mdio *mdio, int phy_addr,
				   int reg_addr)
{
	u32 ctrl = readl(mdio->base + SD5113_MDIO_CTRL);

	ctrl &= ~SD5113_MDIO_CTRL_PHYADDR_MASK;
	ctrl |= ((u32)phy_addr << SD5113_MDIO_CTRL_PHYADDR_SHIFT) &
		SD5113_MDIO_CTRL_PHYADDR_MASK;
	ctrl &= ~SD5113_MDIO_CTRL_REGADDR_MASK;
	ctrl |= (u32)reg_addr & SD5113_MDIO_CTRL_REGADDR_MASK;

	return ctrl;
}

static int sd5113_mdio_read(struct mii_bus *bus, int phy_addr, int reg_addr)
{
	struct sd5113_mdio *mdio = bus->priv;
	u32 ctrl;
	int ret;

	/* A previous timed-out command may still own the controller. */
	ret = sd5113_mdio_wait_ready(mdio);
	if (ret)
		return ret;

	/*
	 * CONFIRMED sequence (hi_kernel_mdio_read): build the address
	 * fields, clear the write bit, set the read bit + go bit, wait for
	 * completion, then read the result from the data register's upper
	 * 16 bits.
	 */
	ctrl = sd5113_mdio_build_ctrl(mdio, phy_addr, reg_addr);
	ctrl &= ~SD5113_MDIO_CTRL_WRITE;
	ctrl |= SD5113_MDIO_CTRL_READ | SD5113_MDIO_CTRL_GO;
	writel(ctrl, mdio->base + SD5113_MDIO_CTRL);

	ret = sd5113_mdio_wait_ready(mdio);
	if (ret)
		return ret;

	return readl(mdio->base + SD5113_MDIO_DATA) >> SD5113_MDIO_DATA_SHIFT;
}

static int sd5113_mdio_write(struct mii_bus *bus, int phy_addr, int reg_addr,
			      u16 val)
{
	struct sd5113_mdio *mdio = bus->priv;
	u32 ctrl, data;
	int ret;

	/* Do not overwrite even the data latch while a command is busy. */
	ret = sd5113_mdio_wait_ready(mdio);
	if (ret)
		return ret;

	data = readl(mdio->base + SD5113_MDIO_DATA);
	data = (data & GENMASK(31, 16)) | (u32)val;
	writel(data, mdio->base + SD5113_MDIO_DATA);

	ctrl = sd5113_mdio_build_ctrl(mdio, phy_addr, reg_addr);
	ctrl &= ~SD5113_MDIO_CTRL_READ;
	ctrl |= SD5113_MDIO_CTRL_WRITE | SD5113_MDIO_CTRL_GO;
	writel(ctrl, mdio->base + SD5113_MDIO_CTRL);

	ret = sd5113_mdio_wait_ready(mdio);
	if (ret)
		return ret;

	return 0;
}

static int sd5113_mdio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sd5113_mdio *mdio;
	struct mii_bus *bus;
	int ret;

	mdio = devm_kzalloc(dev, sizeof(*mdio), GFP_KERNEL);
	if (!mdio)
		return -ENOMEM;

	mdio->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mdio->base))
		return PTR_ERR(mdio->base);

	mdio->crg = syscon_regmap_lookup_by_phandle(dev->of_node, "hisilicon,crg");
	if (IS_ERR(mdio->crg))
		return dev_err_probe(dev, PTR_ERR(mdio->crg), "cannot get CRG\n");

	bus = devm_mdiobus_alloc(dev);
	if (!bus)
		return -ENOMEM;

	ret = sd5113_mdio_hw_enable(mdio);
	if (ret)
		return dev_err_probe(dev, ret, "cannot enable MDIO\n");

	bus->name = "sd5113_mdio";
	snprintf(bus->id, MII_BUS_ID_SIZE, "%s", dev_name(dev));
	bus->read = sd5113_mdio_read;
	bus->write = sd5113_mdio_write;
	bus->parent = dev;
	bus->priv = mdio;

	/* Unregister children and bus before devm frees bus/MMIO resources. */
	ret = devm_of_mdiobus_register(dev, bus, dev->of_node);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, bus);

	return 0;
}

static const struct of_device_id sd5113_mdio_of_match[] = {
	{ .compatible = "hisilicon,sd5113-mdio" },
	{ }
};
MODULE_DEVICE_TABLE(of, sd5113_mdio_of_match);

static struct platform_driver sd5113_mdio_driver = {
	.probe = sd5113_mdio_probe,
	.driver = {
		.name = "sd5113-mdio",
		.of_match_table = sd5113_mdio_of_match,
	},
};
module_platform_driver(sd5113_mdio_driver);

MODULE_DESCRIPTION("HiSilicon SD5113 MDIO bus driver");
MODULE_LICENSE("GPL");
