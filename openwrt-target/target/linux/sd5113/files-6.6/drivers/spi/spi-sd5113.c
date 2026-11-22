// SPDX-License-Identifier: GPL-2.0-only
#include <linux/clk.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/sd5113.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/spi/spi.h>

#define SD_SPI_FORMAT	0x00
#define SD_SPI_ENABLE	0x08
#define SD_SPI_SELECT	0x10
#define SD_SPI_DIV	0x14
#define SD_SPI_STATUS	0x28
#define SD_SPI_IRQ	0x2c
#define SD_SPI_DATA	0x60
#define SD_SPI_RUN	BIT(0)
#define SD_SPI_TX	BIT(8)
#define SD_SPI_RX	BIT(9)
#define SD_SPI_BUSY	BIT(0)
#define SD_SPI_TX_READY	BIT(1)
#define SD_SPI_TX_EMPTY	BIT(2)
#define SD_SPI_RX_READY	BIT(3)
#define SD_SPI_FRAME	(GENMASK(7, 0) | BIT(11))
#define SD_SPI_STOCK_8	0xc7
#define SD_SPI_SEL_MASK	GENMASK(15, 0)
#define SD_SPI_DIV_MASK	GENMASK(15, 0)
#define SD_SPI_IRQ_MASK	GENMASK(5, 0)
#define SD_SPI_MAX_HZ	8000000U
#define SD_SPI_MAX_DIV	0xfffeU
#define SD_SPI_WAIT_US	500
#define SD_SPI_MAX_MSG	17

#define SD_CRG_CTRL	0x1c
#define SD_CRG_SET	0x24
#define SD_CRG_CLEAR	0x28
#define SD_CRG_STATUS	0x2c
#define SD_CRG_ROUTE	0x40
#define SD_CRG_SPI_GATE	BIT(26)
#define SD_CRG_SPI_MUX	BIT(21)
#define SD_SPI_BUS_PADS	GENMASK(2, 0)
#define SD_SPI_CS0_PAD	BIT(3)
#define SD_SPI_CS0_PADS	(SD_SPI_BUS_PADS | SD_SPI_CS0_PAD)
#define SD_SPI_CS1_PAD	BIT(4)

struct sd5113_spi {
	struct device *dev;
	void __iomem *base;
	struct regmap *crg;
	struct gpio_descs *pads;
	unsigned long ahb;
	u32 old_ctrl, old_route, old_funcs;
	u32 old_format, old_cs, old_div, old_irq;
	bool old_clock;
	u32 divisor;
};

static void sd_spi_rmw(struct sd5113_spi *ss, u32 offset, u32 mask, u32 value)
{
	u32 old = readl(ss->base + offset);

	writel((old & ~mask) | (value & mask), ss->base + offset);
}

static int sd_spi_gate(struct sd5113_spi *ss, bool on)
{
	u32 status;
	int ret;

	ret = regmap_write(ss->crg, on ? SD_CRG_SET : SD_CRG_CLEAR, SD_CRG_SPI_GATE);
	if (ret)
		return ret;
	return regmap_read_poll_timeout(ss->crg, SD_CRG_STATUS, status,
					!!(status & SD_CRG_SPI_GATE) == on, 5, 1000);
}

static int sd_spi_divisor(unsigned long ahb, u32 speed, u32 *divisor)
{
	unsigned long div;

	if (!speed || speed > SD_SPI_MAX_HZ)
		return -EINVAL;
	div = round_up(DIV_ROUND_UP(ahb, speed), 2);
	if (div < 2 || div > SD_SPI_MAX_DIV)
		return -ERANGE;
	*divisor = div;
	return 0;
}

static int sd_spi_program(struct sd5113_spi *ss, u32 divisor)
{
	u32 ctrl;
	int ret;

	sd_spi_rmw(ss, SD_SPI_ENABLE, SD_SPI_RUN, 0);
	sd_spi_rmw(ss, SD_SPI_DIV, SD_SPI_DIV_MASK, divisor);
	sd_spi_rmw(ss, SD_SPI_FORMAT, SD_SPI_FRAME, SD_SPI_STOCK_8);
	sd_spi_rmw(ss, SD_SPI_SELECT, SD_SPI_SEL_MASK, BIT(0));
	ret = regmap_set_bits(ss->crg, SD_CRG_CTRL, SD_CRG_SPI_MUX);
	ret = ret ?: regmap_read(ss->crg, SD_CRG_CTRL, &ctrl);
	if (ret)
		return ret;
	if (!(ctrl & SD_CRG_SPI_MUX) ||
	    (readl(ss->base + SD_SPI_DIV) & SD_SPI_DIV_MASK) != divisor ||
	    (readl(ss->base + SD_SPI_FORMAT) & SD_SPI_FRAME) != SD_SPI_STOCK_8 ||
	    (readl(ss->base + SD_SPI_SELECT) & SD_SPI_SEL_MASK) != BIT(0))
		return -EIO;

	ss->divisor = divisor;
	return 0;
}

static int sd_spi_funcs(struct sd5113_spi *ss, u32 mask, u32 value)
{
	if (mask & ~SD_SPI_CS0_PADS)
		return -EINVAL;
	return sd5113_gpio_set_funcs(ss->pads->desc, ss->pads->ndescs, mask, value, NULL);
}

static int sd_spi_check_cs0_pads(struct sd5113_spi *ss, u32 *owned)
{
	u32 funcs;
	int ret;

	ret = sd5113_gpio_read_bank_funcs(ss->pads->desc[0], &funcs);
	if (ret)
		return ret;
	/* Stock CS0 leaves GPIO4 in GPIO mode; the WLAN LED may own it. */
	if (funcs & SD_SPI_CS1_PAD) {
		dev_warn(ss->dev, "GPIO4 FUNC[4] is set; CS0 requires GPIO4 in GPIO mode\n");
		return -EBUSY;
	}
	if (owned)
		*owned = funcs & SD_SPI_CS0_PADS;
	return 0;
}

static int sd_spi_verify_route(struct sd5113_spi *ss)
{
	u32 route, funcs;
	int ret;

	ret = regmap_read(ss->crg, SD_CRG_ROUTE, &route);
	ret = ret ?: sd_spi_check_cs0_pads(ss, &funcs);
	if (ret)
		return ret;
	if (!(route & SD_CRG_SPI_MUX) || funcs != SD_SPI_CS0_PADS) {
		dev_warn(ss->dev, "SPI pad route lost: CRG40=%08x GPIO0_FUNC[3:0]=%02x\n",
			 route, funcs);
		return -EIO;
	}
	return 0;
}

static void sd_spi_restore(void *data)
{
	struct sd5113_spi *ss = data;
	u32 status;
	int ret, failed = 0;

	ret = regmap_read(ss->crg, SD_CRG_STATUS, &status);
	failed |= ret;
	if (!ret && (status & SD_CRG_SPI_GATE))
		sd_spi_rmw(ss, SD_SPI_ENABLE, SD_SPI_RUN, 0);
	if (ss->old_clock) {
		ret = sd_spi_gate(ss, true);
		failed |= ret;
		if (!ret) {
			sd_spi_rmw(ss, SD_SPI_ENABLE, SD_SPI_RUN, 0);
			sd_spi_rmw(ss, SD_SPI_FORMAT, SD_SPI_FRAME | SD_SPI_TX | SD_SPI_RX,
				   ss->old_format);
			sd_spi_rmw(ss, SD_SPI_SELECT, SD_SPI_SEL_MASK, ss->old_cs);
			sd_spi_rmw(ss, SD_SPI_DIV, SD_SPI_DIV_MASK, ss->old_div);
			sd_spi_rmw(ss, SD_SPI_IRQ, SD_SPI_IRQ_MASK, ss->old_irq);
		}
	}
	failed |= regmap_update_bits(ss->crg, SD_CRG_CTRL, SD_CRG_SPI_MUX, ss->old_ctrl);
	failed |= sd_spi_funcs(ss, SD_SPI_CS0_PADS, ss->old_funcs);
	failed |= regmap_update_bits(ss->crg, SD_CRG_ROUTE, SD_CRG_SPI_MUX, ss->old_route);
	failed |= sd_spi_gate(ss, ss->old_clock);
	if (failed)
		dev_warn(ss->dev, "SPI state restoration incomplete\n");
}

static int sd_spi_start(struct sd5113_spi *ss, u32 divisor)
{
	u32 status;
	int ret;

	ret = regmap_read(ss->crg, SD_CRG_CTRL, &ss->old_ctrl);
	ret = ret ?: regmap_read(ss->crg, SD_CRG_ROUTE, &ss->old_route);
	ret = ret ?: regmap_read(ss->crg, SD_CRG_STATUS, &status);
	ret = ret ?: sd_spi_check_cs0_pads(ss, &ss->old_funcs);
	if (ret)
		return ret;
	ss->old_clock = !!(status & SD_CRG_SPI_GATE);
	if (ss->old_clock) {
		if ((readl(ss->base + SD_SPI_ENABLE) & SD_SPI_RUN) ||
		    (readl(ss->base + SD_SPI_STATUS) & (SD_SPI_BUSY | SD_SPI_RX_READY)))
			return -EBUSY;
		ss->old_format = readl(ss->base + SD_SPI_FORMAT);
		ss->old_cs = readl(ss->base + SD_SPI_SELECT);
		ss->old_div = readl(ss->base + SD_SPI_DIV);
		ss->old_irq = readl(ss->base + SD_SPI_IRQ);
	}
	ret = devm_add_action(ss->dev, sd_spi_restore, ss);
	if (ret)
		return ret;

	ret = sd_spi_gate(ss, false);
	ret = ret ?: regmap_clear_bits(ss->crg, SD_CRG_CTRL, SD_CRG_SPI_MUX);
	ret = ret ?: sd_spi_funcs(ss, SD_SPI_CS0_PAD, SD_SPI_CS0_PAD);
	ret = ret ?: regmap_clear_bits(ss->crg, SD_CRG_ROUTE, SD_CRG_SPI_MUX);
	ret = ret ?: sd_spi_funcs(ss, SD_SPI_CS0_PADS, 0);
	ret = ret ?: sd_spi_gate(ss, false);
	ret = ret ?: sd_spi_funcs(ss, SD_SPI_CS0_PAD, SD_SPI_CS0_PAD);
	ret = ret ?: regmap_set_bits(ss->crg, SD_CRG_ROUTE, SD_CRG_SPI_MUX);
	ret = ret ?: sd_spi_funcs(ss, SD_SPI_BUS_PADS, SD_SPI_BUS_PADS);
	ret = ret ?: sd_spi_gate(ss, true);
	ret = ret ?: sd_spi_verify_route(ss);
	if (ret)
		return ret;

	sd_spi_rmw(ss, SD_SPI_IRQ, SD_SPI_IRQ_MASK, 0);
	return sd_spi_program(ss, divisor);
}

static int sd_spi_byte(struct sd5113_spi *ss, const u8 *tx, u8 *rx)
{
	u32 status;
	int ret;

	if (!!tx == !!rx)
		return -EINVAL;
	sd_spi_rmw(ss, SD_SPI_FORMAT, SD_SPI_TX | SD_SPI_RX, tx ? SD_SPI_TX : SD_SPI_RX);
	sd_spi_rmw(ss, SD_SPI_ENABLE, SD_SPI_RUN, SD_SPI_RUN);
	if (tx) {
		ret = readl_poll_timeout_atomic(ss->base + SD_SPI_STATUS, status,
						status & SD_SPI_TX_READY, 1, SD_SPI_WAIT_US);
		if (!ret) {
			writel(*tx, ss->base + SD_SPI_DATA);
			ret = readl_poll_timeout_atomic(ss->base + SD_SPI_STATUS, status,
					(status & (SD_SPI_TX_EMPTY | SD_SPI_BUSY)) == SD_SPI_TX_EMPTY,
					1, SD_SPI_WAIT_US);
		}
	} else {
		writel(0xffff, ss->base + SD_SPI_DATA);
		ret = readl_poll_timeout_atomic(ss->base + SD_SPI_STATUS, status,
						status & SD_SPI_RX_READY, 1, SD_SPI_WAIT_US);
		if (!ret)
			*rx = readl(ss->base + SD_SPI_DATA) & 0xff;
	}
	sd_spi_rmw(ss, SD_SPI_ENABLE, SD_SPI_RUN, 0);
	return ret;
}

static int sd_spi_setup(struct spi_device *spi)
{
	/* Linux mode 3 selects the stock 0xc7 format; edge timing is unverified. */
	return spi->mode == SPI_MODE_3 && spi->bits_per_word == 8 ? 0 : -EINVAL;
}

static size_t sd_spi_max_size(struct spi_device *spi)
{
	return SD_SPI_MAX_MSG;
}

static int sd_spi_idle(struct sd5113_spi *ss)
{
	u32 status;
	int ret;

	ret = regmap_read(ss->crg, SD_CRG_STATUS, &status);
	if (ret || !(status & SD_CRG_SPI_GATE))
		return ret ?: -EIO;
	if ((readl(ss->base + SD_SPI_ENABLE) & SD_SPI_RUN) ||
	    (readl(ss->base + SD_SPI_STATUS) & (SD_SPI_BUSY | SD_SPI_RX_READY)))
		return -EBUSY;
	return 0;
}

static int sd_spi_reselect(struct sd5113_spi *ss, u32 divisor)
{
	int ret, recover;

	ret = sd_spi_idle(ss);
	ret = ret ?: sd_spi_check_cs0_pads(ss, NULL);
	if (ret)
		return ret;
	ret = regmap_clear_bits(ss->crg, SD_CRG_CTRL, SD_CRG_SPI_MUX);
	ret = ret ?: sd_spi_funcs(ss, SD_SPI_CS0_PAD, SD_SPI_CS0_PAD);
	ret = ret ?: sd_spi_program(ss, divisor);
	if (ret) {
		ss->divisor = 0;
		recover = regmap_set_bits(ss->crg, SD_CRG_CTRL, SD_CRG_SPI_MUX);
		if (recover)
			dev_warn(ss->dev, "SPI configuration recovery failed: %d\n", recover);
	}
	return ret;
}

static int sd_spi_prepare_message(struct spi_controller *ctlr, struct spi_message *message)
{
	struct sd5113_spi *ss = spi_controller_get_devdata(ctlr);
	struct spi_transfer *xfer;
	u32 divisor, speed;
	unsigned int count = 0;
	int ret;

	if (list_empty(&message->transfers))
		return -EINVAL;
	xfer = list_first_entry(&message->transfers, struct spi_transfer, transfer_list);
	speed = xfer->speed_hz;
	list_for_each_entry(xfer, &message->transfers, transfer_list) {
		bool last = list_is_last(&xfer->transfer_list, &message->transfers);

		/* Hardware RUN ends each byte. Reject any request to hold CS. */
		if (++count > SD_SPI_MAX_MSG || xfer->len != 1 ||
		    !!xfer->tx_buf == !!xfer->rx_buf || xfer->cs_off ||
		    xfer->cs_change != !last || xfer->speed_hz != speed)
			return -EOPNOTSUPP;
	}
	ret = sd_spi_divisor(ss->ahb, speed, &divisor);
	if (ret)
		return ret;
	if (count > 1 || divisor != ss->divisor)
		ret = sd_spi_reselect(ss, divisor);
	return ret ?: sd_spi_verify_route(ss);
}

static int sd_spi_transfer_one(struct spi_controller *ctlr, struct spi_device *spi,
			       struct spi_transfer *xfer)
{
	struct sd5113_spi *ss = spi_controller_get_devdata(ctlr);
	int ret = sd_spi_idle(ss);

	if (ret || !ss->divisor)
		return ret ?: -EIO;
	xfer->effective_speed_hz = ss->ahb / ss->divisor;
	return sd_spi_byte(ss, xfer->tx_buf, xfer->rx_buf);
}

static int sd_spi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct spi_controller *ctlr;
	struct sd5113_spi *ss;
	struct clk *ahb;
	struct gpio_chip *chip;
	unsigned int i;
	u32 divisor;
	int ret;

	ctlr = devm_spi_alloc_host(dev, sizeof(*ss));
	if (!ctlr)
		return -ENOMEM;
	ss = spi_controller_get_devdata(ctlr);
	ss->dev = dev;
	ss->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ss->base))
		return PTR_ERR(ss->base);
	ss->crg = syscon_regmap_lookup_by_phandle(dev->of_node, "hisilicon,crg");
	if (IS_ERR(ss->crg))
		return dev_err_probe(dev, PTR_ERR(ss->crg), "missing CRG\n");
	ss->pads = devm_gpiod_get_array(dev, "spi-pads", GPIOD_ASIS);
	if (IS_ERR(ss->pads))
		return dev_err_probe(dev, PTR_ERR(ss->pads), "cannot reserve SPI pads\n");
	if (ss->pads->ndescs != 4)
		return dev_err_probe(dev, -EINVAL, "CS0 needs four GPIOs; do not reserve GPIO4\n");
	chip = gpiod_to_chip(ss->pads->desc[0]);
	if (!chip)
		return -EINVAL;
	for (i = 0; i < ss->pads->ndescs; i++)
		if (gpiod_to_chip(ss->pads->desc[i]) != chip ||
		    desc_to_gpio(ss->pads->desc[i]) != chip->base + i)
			return dev_err_probe(dev, -EINVAL, "expected GPIO0[3:0] SPI pads\n");
	ahb = devm_clk_get_enabled(dev, "ahb");
	if (IS_ERR(ahb))
		return dev_err_probe(dev, PTR_ERR(ahb), "missing AHB clock\n");
	ss->ahb = clk_get_rate(ahb);
	ret = sd_spi_divisor(ss->ahb, SD_SPI_MAX_HZ, &divisor);
	ret = ret ?: sd_spi_start(ss, divisor);
	if (ret)
		return dev_err_probe(dev, ret, "SPI controller initialization failed\n");

	ctlr->dev.of_node = dev->of_node;
	ctlr->num_chipselect = 1;
	ctlr->mode_bits = SPI_MODE_3;
	ctlr->bits_per_word_mask = SPI_BPW_MASK(8);
	ctlr->flags = SPI_CONTROLLER_HALF_DUPLEX;
	ctlr->min_speed_hz = DIV_ROUND_UP(ss->ahb, SD_SPI_MAX_DIV);
	ctlr->max_speed_hz = SD_SPI_MAX_HZ;
	ctlr->setup = sd_spi_setup;
	ctlr->max_message_size = sd_spi_max_size;
	ctlr->prepare_message = sd_spi_prepare_message;
	ctlr->transfer_one = sd_spi_transfer_one;

	ret = devm_spi_register_controller(dev, ctlr);
	if (ret)
		return dev_err_probe(dev, ret, "cannot register SPI controller\n");
	dev_info(dev, "SPI master registered: CS0 on GPIO0[3:0], GPIO4 not owned; divider %u\n",
		 divisor);
	return 0;
}

static const struct of_device_id sd_spi_of_match[] = {
	{ .compatible = "hisilicon,sd5113-spi" },
	{ }
};
MODULE_DEVICE_TABLE(of, sd_spi_of_match);

static struct platform_driver sd_spi_driver = {
	.probe = sd_spi_probe,
	.driver = {
		.name = "spi-sd5113",
		.of_match_table = sd_spi_of_match,
	},
};
module_platform_driver(sd_spi_driver);

MODULE_DESCRIPTION("HiSilicon SD5113 SPI controller");
MODULE_LICENSE("GPL");
