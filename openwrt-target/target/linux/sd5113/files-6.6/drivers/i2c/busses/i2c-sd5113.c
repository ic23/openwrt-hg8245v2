// SPDX-License-Identifier: GPL-2.0-only
/* HiSilicon SD5113 native I2C controller */

#include <linux/clk.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/sd5113.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>

#define SD_I2C_CON			0x00
#define   SD_CON_MASTER			BIT(0)
#define   SD_CON_STD			BIT(1)
#define   SD_CON_RESTART		BIT(5)
#define   SD_CON_SLAVE_DISABLE		BIT(6)
#define SD_I2C_TAR			0x04
#define SD_I2C_DATA_CMD			0x10
#define   SD_CMD_READ			BIT(8)
#define   SD_CMD_STOP			BIT(9)
#define   SD_CMD_RESTART		BIT(10)
#define SD_I2C_SS_HCNT			0x14
#define SD_I2C_SS_LCNT			0x18
#define SD_I2C_INTR_MASK		0x30
#define SD_I2C_RAW_INTR			0x34
#define   SD_INTR_FIFO_ERR		(BIT(0) | BIT(1) | BIT(3))
#define   SD_INTR_TX_ABRT		BIT(6)
#define   SD_INTR_STOP			BIT(9)
#define SD_I2C_CLR_INTR			0x40
#define SD_I2C_ENABLE			0x6c
#define   SD_ENABLE			BIT(0)
#define   SD_ABORT			BIT(1)
#define SD_I2C_STATUS			0x70
#define   SD_STATUS_ACTIVITY		BIT(0)
#define   SD_STATUS_TFE			BIT(2)
#define   SD_STATUS_RFNE		BIT(3)
#define   SD_STATUS_MASTER_ACTIVE	BIT(5)
#define SD_I2C_RXFLR			0x78
#define SD_I2C_TX_ABRT_SOURCE		0x80
#define   SD_ABRT_ADDR_NACK		BIT(0)
#define   SD_ABRT_DATA_NACK		BIT(3)
#define   SD_ABRT_ARB_LOST		BIT(12)
#define SD_I2C_ENABLE_STATUS		0x9c
#define SD_I2C_COMP_PARAM		0xf4
#define   SD_PARAM_ENCODED		BIT(7)
#define SD_I2C_COMP_TYPE		0xfc
#define   SD_COMP_TYPE_DW		0x44570140

#define SD_I2C_CRG_ROUTE		0x1c
#define SD_I2C_CRG_ROUTE_BIT		BIT(22)
#define SD_I2C_CRG_GATE_STATUS		0x2c
#define SD_I2C_CRG_GATE_BIT		BIT(27)
#define SD_I2C_PADS			GENMASK(6, 5)
#define SD_I2C_STD_HZ			100000
#define SD_I2C_MAX_CMDS			64
#define SD_I2C_POLL_US			10
#define SD_I2C_IDLE_US			20000

struct sd5113_i2c {
	struct device *dev;
	void __iomem *base;
	struct clk *clk;
	struct regmap *crg;
	struct gpio_descs *pads;
	struct i2c_adapter adap;
	struct i2c_adapter_quirks quirks;
	u32 old_func;
	u32 old_route;
	unsigned int tx_depth;
	unsigned int rx_depth;
	bool func_changed;
	bool route_changed;
	bool clock_on;
	bool identified;
	bool failed;
};

static void sd_i2c_update_count(struct sd5113_i2c *i2c, u32 reg, u32 count)
{
	/* Keep the upper half as in the stock divisor programming. */
	writel((readl(i2c->base + reg) & GENMASK(31, 16)) | count,
	       i2c->base + reg);
}

static int sd_i2c_divisor(unsigned long rate, u32 *count)
{
	unsigned long div = rate / (2 * SD_I2C_STD_HZ);

	div += rate % (2 * SD_I2C_STD_HZ) != 0;
	if (div < 8 || div > U16_MAX)
		return -ERANGE;

	*count = div;
	return 0;
}

static int sd_i2c_enable(struct sd5113_i2c *i2c, bool enable)
{
	u32 want = enable ? SD_ENABLE : 0;
	u32 val;

	writel(want, i2c->base + SD_I2C_ENABLE);
	return readl_poll_timeout(i2c->base + SD_I2C_ENABLE_STATUS, val,
				  (val & SD_ENABLE) == want,
				  SD_I2C_POLL_US, SD_I2C_IDLE_US);
}

static int sd_i2c_configure(struct sd5113_i2c *i2c, u32 count)
{
	u32 con = SD_CON_MASTER | SD_CON_RESTART | SD_CON_SLAVE_DISABLE | SD_CON_STD;
	int ret;

	ret = sd_i2c_enable(i2c, false);
	if (ret)
		return ret;

	writel(0, i2c->base + SD_I2C_INTR_MASK);
	readl(i2c->base + SD_I2C_CLR_INTR);
	sd_i2c_update_count(i2c, SD_I2C_SS_LCNT, count);
	sd_i2c_update_count(i2c, SD_I2C_SS_HCNT, count);
	writel(con, i2c->base + SD_I2C_CON);
	return readl(i2c->base + SD_I2C_CON) == con ? 0 : -EIO;
}

/*
 * Bound one transfer to one TX FIFO load and fit all replies in the RX
 * FIFO. No interrupts or scheduler latency between refills are needed,
 * including on DesignWare builds which stop when the TX FIFO runs dry.
 */
static int sd_i2c_commands(struct sd5113_i2c *i2c, const struct i2c_msg *msgs,
			   int num, u16 *cmds, unsigned int *reads)
{
	unsigned int total = 0;
	int i, j;

	*reads = 0;
	if (num < 1 || num > 2)
		return -EOPNOTSUPP;
	if (num == 2 && ((msgs[0].flags & I2C_M_RD) ||
			 !(msgs[1].flags & I2C_M_RD) ||
			 msgs[0].addr != msgs[1].addr))
		return -EOPNOTSUPP;

	for (i = 0; i < num; i++) {
		const struct i2c_msg *msg = &msgs[i];
		bool read = msg->flags & I2C_M_RD;

		if (msg->flags & ~(I2C_M_RD | I2C_M_DMA_SAFE) || !msg->len ||
		    msg->len > i2c->tx_depth - total)
			return -EOPNOTSUPP;
		if (msg->addr > 0x7f || !msg->buf)
			return -EINVAL;
		if (read) {
			*reads += msg->len;
			if (*reads > i2c->rx_depth)
				return -EOPNOTSUPP;
		}
		for (j = 0; j < msg->len; j++)
			cmds[total++] = read ? SD_CMD_READ : msg->buf[j];
		if (i)
			cmds[total - msg->len] |= SD_CMD_RESTART;
	}
	cmds[total - 1] |= SD_CMD_STOP;
	return total;
}

static int sd_i2c_abort_errno(u32 source)
{
	if (source & SD_ABRT_ARB_LOST)
		return -EAGAIN;
	if (source & SD_ABRT_ADDR_NACK)
		return -ENXIO;
	if (source & SD_ABRT_DATA_NACK)
		return -EREMOTEIO;
	return -EIO;
}

static int sd_i2c_stop(struct sd5113_i2c *i2c, bool abort)
{
	u32 val;
	int ret;

	if (abort && (readl(i2c->base + SD_I2C_ENABLE_STATUS) & SD_ENABLE) &&
	    (readl(i2c->base + SD_I2C_STATUS) & SD_STATUS_MASTER_ACTIVE)) {
		writel(SD_ENABLE | SD_ABORT, i2c->base + SD_I2C_ENABLE);
		readl_poll_timeout(i2c->base + SD_I2C_ENABLE, val,
				   !(val & SD_ABORT), SD_I2C_POLL_US, 2000);
	}

	ret = sd_i2c_enable(i2c, false);
	if (ret) {
		i2c->failed = true;
		dev_err_ratelimited(i2c->dev, "controller did not disable\n");
	} else {
		readl(i2c->base + SD_I2C_CLR_INTR);
	}
	return ret;
}

static int sd_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	struct sd5113_i2c *i2c = i2c_get_adapdata(adap);
	unsigned int reads, timeout;
	unsigned long flags;
	u16 cmds[SD_I2C_MAX_CMDS];
	u32 val;
	int count, i, ret, stop_ret;
	u8 *rx;

	count = sd_i2c_commands(i2c, msgs, num, cmds, &reads);
	if (count < 0)
		return count;
	if (i2c->failed)
		return -EIO;

	ret = readl_poll_timeout(i2c->base + SD_I2C_STATUS, val,
				 !(val & SD_STATUS_ACTIVITY),
				 SD_I2C_POLL_US, SD_I2C_IDLE_US);
	if (ret)
		return ret;

	writel(msgs[0].addr, i2c->base + SD_I2C_TAR);
	readl(i2c->base + SD_I2C_CLR_INTR);
	ret = sd_i2c_enable(i2c, true);
	if (ret)
		goto out_stop;

	/* A short, fixed batch of MMIO writes; never poll or sleep here. */
	local_irq_save(flags);
	for (i = 0; i < count; i++)
		writel_relaxed(cmds[i], i2c->base + SD_I2C_DATA_CMD);
	local_irq_restore(flags);

	/* Nine standard-mode clocks per command byte and address phase. */
	timeout = SD_I2C_IDLE_US + (count + num) * 90U;
	ret = readl_poll_timeout(i2c->base + SD_I2C_RAW_INTR, val,
				 val & (SD_INTR_TX_ABRT | SD_INTR_STOP | SD_INTR_FIFO_ERR),
				 SD_I2C_POLL_US, timeout);
	if (ret)
		goto out_stop;
	if (val & SD_INTR_TX_ABRT) {
		ret = sd_i2c_abort_errno(readl(i2c->base + SD_I2C_TX_ABRT_SOURCE));
		goto out_stop;
	}
	if (val & SD_INTR_FIFO_ERR) {
		ret = -EIO;
		goto out_stop;
	}

	ret = readl_poll_timeout(i2c->base + SD_I2C_STATUS, val,
				 (val & (SD_STATUS_ACTIVITY | SD_STATUS_TFE)) == SD_STATUS_TFE,
				 SD_I2C_POLL_US, SD_I2C_IDLE_US);
	if (ret)
		goto out_stop;
	if (readl(i2c->base + SD_I2C_RXFLR) != reads) {
		ret = -EIO;
		goto out_stop;
	}

	rx = msgs[num - 1].buf;
	for (i = 0; i < reads; i++) {
		if (!(readl(i2c->base + SD_I2C_STATUS) & SD_STATUS_RFNE)) {
			ret = -EIO;
			goto out_stop;
		}
		rx[i] = readl(i2c->base + SD_I2C_DATA_CMD);
	}

out_stop:
	stop_ret = sd_i2c_stop(i2c, ret && ret != -EAGAIN);
	return stop_ret ?: (ret ?: num);
}

static u32 sd_i2c_functionality(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C;
}

static const struct i2c_algorithm sd_i2c_algorithm = {
	.master_xfer = sd_i2c_xfer,
	.functionality = sd_i2c_functionality,
};

static void sd_i2c_shutdown(void *data)
{
	struct sd5113_i2c *i2c = data;
	int ret;

	if (i2c->identified)
		sd_i2c_stop(i2c, true);
	if (i2c->route_changed) {
		ret = regmap_clear_bits(i2c->crg, SD_I2C_CRG_ROUTE, SD_I2C_CRG_ROUTE_BIT);
		if (ret)
			dev_warn(i2c->dev, "cannot clear I2C route: %d\n", ret);
	}
	if (i2c->func_changed) {
		ret = sd5113_gpio_set_funcs(i2c->pads->desc, i2c->pads->ndescs,
					    SD_I2C_PADS, i2c->old_func, NULL);
		if (ret)
			dev_warn(i2c->dev, "cannot restore I2C pads: %d\n", ret);
	}
	if (i2c->clock_on)
		clk_disable_unprepare(i2c->clk);
	if (i2c->route_changed) {
		ret = regmap_update_bits(i2c->crg, SD_I2C_CRG_ROUTE,
					 SD_I2C_CRG_ROUTE_BIT, i2c->old_route);
		if (ret)
			dev_warn(i2c->dev, "cannot restore I2C route: %d\n", ret);
	}
}

static int sd_i2c_start(struct sd5113_i2c *i2c, u32 count)
{
	u32 params, value;
	int ret;

	ret = regmap_read(i2c->crg, SD_I2C_CRG_ROUTE, &value);
	if (ret)
		return ret;
	i2c->old_route = value & SD_I2C_CRG_ROUTE_BIT;
	i2c->route_changed = true;
	ret = regmap_clear_bits(i2c->crg, SD_I2C_CRG_ROUTE, SD_I2C_CRG_ROUTE_BIT);
	if (ret)
		return ret;
	ret = regmap_read(i2c->crg, SD_I2C_CRG_ROUTE, &value);
	if (ret || (value & SD_I2C_CRG_ROUTE_BIT))
		return ret ?: -EIO;
	ret = sd5113_gpio_set_funcs(i2c->pads->desc, i2c->pads->ndescs,
				    SD_I2C_PADS, SD_I2C_PADS, &i2c->old_func);
	if (ret)
		return ret;
	i2c->func_changed = true;
	ret = sd5113_gpio_set_funcs(i2c->pads->desc, i2c->pads->ndescs, 0, 0, &value);
	if (ret || value != SD_I2C_PADS)
		return ret ?: -EIO;

	ret = clk_prepare_enable(i2c->clk);
	if (ret)
		return ret;
	i2c->clock_on = true;
	ret = regmap_read(i2c->crg, SD_I2C_CRG_GATE_STATUS, &value);
	if (ret)
		return ret;
	if (!(value & SD_I2C_CRG_GATE_BIT))
		return -EIO;
	value = readl(i2c->base + SD_I2C_COMP_TYPE);
	if (value != SD_COMP_TYPE_DW) {
		dev_err(i2c->dev, "unexpected I2C component ID %#010x\n", value);
		return -ENODEV;
	}

	i2c->identified = true;
	params = readl(i2c->base + SD_I2C_COMP_PARAM);
	if (params & SD_PARAM_ENCODED) {
		i2c->tx_depth = min(1 + ((params >> 16) & 0xff), SD_I2C_MAX_CMDS);
		i2c->rx_depth = min(1 + ((params >> 8) & 0xff), SD_I2C_MAX_CMDS);
	} else {
		/* The minimum DesignWare FIFO depth; enough for offset + READ. */
		i2c->tx_depth = 2;
		i2c->rx_depth = 2;
	}
	if (i2c->tx_depth < 2 || i2c->rx_depth < 2)
		return -ENODEV;

	ret = sd_i2c_configure(i2c, count);
	if (ret)
		return ret;
	ret = regmap_set_bits(i2c->crg, SD_I2C_CRG_ROUTE, SD_I2C_CRG_ROUTE_BIT);
	if (ret)
		return ret;
	ret = regmap_read(i2c->crg, SD_I2C_CRG_ROUTE, &value);
	if (ret)
		return ret;
	return value & SD_I2C_CRG_ROUTE_BIT ? 0 : -EIO;
}

static int sd_i2c_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sd5113_i2c *i2c;
	struct resource *res;
	u32 count, freq;
	int ret;

	i2c = devm_kzalloc(dev, sizeof(*i2c), GFP_KERNEL);
	if (!i2c)
		return -ENOMEM;
	i2c->dev = dev;

	i2c->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(i2c->base))
		return PTR_ERR(i2c->base);
	if (resource_size(res) < SD_I2C_COMP_TYPE + sizeof(u32))
		return dev_err_probe(dev, -EINVAL, "incomplete I2C register window\n");
	i2c->crg = syscon_regmap_lookup_by_phandle(dev->of_node, "hisilicon,crg");
	if (IS_ERR(i2c->crg))
		return dev_err_probe(dev, PTR_ERR(i2c->crg), "cannot get CRG\n");
	i2c->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(i2c->clk))
		return dev_err_probe(dev, PTR_ERR(i2c->clk), "cannot get I2C clock\n");
	i2c->pads = devm_gpiod_get_array(dev, "i2c", GPIOD_ASIS);
	if (IS_ERR(i2c->pads))
		return dev_err_probe(dev, PTR_ERR(i2c->pads), "cannot reserve I2C pads\n");
	if (i2c->pads->ndescs != 2)
		return dev_err_probe(dev, -EINVAL, "expected two I2C pads\n");

	if (device_property_present(dev, "clock-frequency")) {
		ret = device_property_read_u32(dev, "clock-frequency", &freq);
		if (ret)
			return dev_err_probe(dev, ret, "invalid clock-frequency\n");
		if (freq != SD_I2C_STD_HZ)
			return dev_err_probe(dev, -EINVAL, "only 100 kHz I2C is supported\n");
	}
	ret = sd_i2c_divisor(clk_get_rate(i2c->clk), &count);
	if (ret)
		return dev_err_probe(dev, ret, "unsupported I2C clock rates\n");

	ret = devm_add_action_or_reset(dev, sd_i2c_shutdown, i2c);
	if (ret)
		return ret;
	ret = sd_i2c_start(i2c, count);
	if (ret)
		return dev_err_probe(dev, ret, "cannot initialize I2C hardware\n");

	i2c->quirks.flags = I2C_AQ_COMB_WRITE_THEN_READ | I2C_AQ_NO_ZERO_LEN;
	i2c->quirks.max_num_msgs = 2;
	i2c->quirks.max_write_len = i2c->tx_depth;
	i2c->quirks.max_read_len = min(i2c->tx_depth, i2c->rx_depth);
	i2c->quirks.max_comb_1st_msg_len = i2c->tx_depth - 1;
	i2c->quirks.max_comb_2nd_msg_len = min(i2c->tx_depth - 1, i2c->rx_depth);
	i2c->adap.owner = THIS_MODULE;
	i2c->adap.algo = &sd_i2c_algorithm;
	i2c->adap.quirks = &i2c->quirks;
	i2c->adap.dev.parent = dev;
	i2c->adap.dev.of_node = dev->of_node;
	strscpy(i2c->adap.name, "SD5113 hardware I2C", sizeof(i2c->adap.name));
	i2c_set_adapdata(&i2c->adap, i2c);
	return devm_i2c_add_adapter(dev, &i2c->adap);
}

static const struct of_device_id sd_i2c_of_match[] = {
	{ .compatible = "hisilicon,sd5113-i2c" },
	{ }
};
MODULE_DEVICE_TABLE(of, sd_i2c_of_match);

static struct platform_driver sd_i2c_driver = {
	.probe = sd_i2c_probe,
	.driver = {
		.name = "sd5113-i2c",
		.of_match_table = sd_i2c_of_match,
	},
};
module_platform_driver(sd_i2c_driver);

MODULE_DESCRIPTION("HiSilicon SD5113 hardware I2C controller");
MODULE_LICENSE("GPL");
