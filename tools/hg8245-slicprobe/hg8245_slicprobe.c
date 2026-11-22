// SPDX-License-Identifier: GPL-2.0-only
#define pr_fmt(fmt) "hg8245-slicprobe: " fmt

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/machine.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/ioport.h>
#include <linux/irqflags.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/regmap.h>

#define SPI_BASE		0x16150000
#define SPI_SIZE		0x64
#define GPIO_BASE		0x16146000
#define GPIO_DATA		0x00
#define GPIO_OUTMODE		0x04
#define GPIO_DIR		0x08
#define GPIO_LEVEL		0x50
#define GPIO2_DIR		(0x2000 + GPIO_DIR)
#define GPIO4			BIT(4)
#define SPI_PAD_COUNT		5
#define SPI_PAD_MASK		GENMASK(4, 0)
#define SPI_PAD_CS0_DIR		GENMASK(3, 0)

#define CRG_CTRL		0x1c
#define CRG_PARENT		0x18
#define CRG_CLK_SET		0x24
#define CRG_CLK_CLEAR		0x28
#define CRG_STATUS		0x2c
#define CRG_HIGHWAY		0x34
#define CRG_ROUTE		0x40
#define CRG_PINMUX		0x54
#define CRG_SPI		BIT(26)
#define CRG_SPI_CTRL		BIT(21)
#define CRG_SPI_ROUTE		BIT(21)

#define SPI_FORMAT		0x00
#define SPI_ENABLE		0x08
#define SPI_SELECT		0x10
#define SPI_DIVISOR		0x14
#define SPI_STATUS		0x28
#define SPI_IRQCFG		0x2c
#define SPI_DATA		0x60
#define SPI_RUN		BIT(0)
#define SPI_TX			BIT(8)
#define SPI_RX			BIT(9)
#define SPI_IDLE_MASK		(BIT(0) | BIT(2))
#define SPI_IDLE		BIT(2)
#define SPI_TX_READY		BIT(1)
#define SPI_RX_READY		BIT(3)
#define SPI_FRAME_MASK		(GENMASK(7, 0) | BIT(11))
#define SPI_FRAME_8BIT		(BIT(7) | BIT(6) | 7)
#define SPI_SELECT_MASK		GENMASK(15, 0)
#define SPI_DIVISOR_MASK	GENMASK(15, 0)
#define SPI_IRQ_MASK		GENMASK(5, 0)
#define SPI_WAIT_US		500
#define SPI_DIV_133MHZ		18

#define SLIC_EC_COMMAND		0x4a
#define SLIC_SYNC_COMMAND	0x06
#define SLIC_ID_COMMAND		0x73
#define SLIC_WRITE_GAP_US	75

#ifdef HG8245_SLICPROBE_UART
#define GPIO_SNAPSHOT_SIZE	(GPIO2_DIR + 4)
#define GPIO4_TRIAL_PINMUX	0xac47e0b5
#else
#define GPIO_SNAPSHOT_SIZE	(GPIO_DIR + 4)
#endif

#ifdef HG8245_SLICPROBE_EMBEDDED
#include "slicprobe-embedded.h"

static const bool tel_unplugged = true;
static const bool bare_id = false;
static const bool stock_ctrl_cycle = true;
#else
static bool tel_unplugged;
module_param(tel_unplugged, bool, 0400);
MODULE_PARM_DESC(tel_unplugged,
		 "Both TEL jacks disconnected: allow one stock preamble and ID read");

static bool bare_id;
module_param(bare_id, bool, 0400);
MODULE_PARM_DESC(bare_id, "Skip the stock MPI preamble for comparison only");

#ifdef HG8245_SLICPROBE_CTRL_TEST
static bool stock_ctrl_cycle;
module_param(stock_ctrl_cycle, bool, 0400);
MODULE_PARM_DESC(stock_ctrl_cycle, "Replay the controller bit transition at each MPI command");
#else
static const bool stock_ctrl_cycle = false;
#endif
#endif

#ifdef HG8245_SLICPROBE_UART
static bool gpio4_low_trial;
module_param(gpio4_low_trial, bool, 0400);
MODULE_PARM_DESC(gpio4_low_trial, "Temporarily use the measured stock GPIO4 level");

static bool stock_cold_cycle;
module_param(stock_cold_cycle, bool, 0400);
MODULE_PARM_DESC(stock_cold_cycle, "Replay the stock SPI clock/route off-on sequence");
#endif

static void replace_bits(void __iomem *reg, u32 mask, u32 value)
{
	u32 old = readl(reg);
	u32 new = (old & ~mask) | (value & mask);

	if (new != old)
		writel(new, reg);
}

static void program_spi_bits(void __iomem *reg, u32 mask, u32 value)
{
	if (stock_ctrl_cycle)
		writel((readl(reg) & ~mask) | (value & mask), reg);
	else
		replace_bits(reg, mask, value);
}

static void set_pad_dir(void __iomem *gpio, u32 dir)
{
	unsigned long flags;

	/* Leased pads; serialize with the HG8245's GPIO RMWs on this UP SoC. */
	local_irq_save(flags);
	replace_bits(gpio + GPIO_DIR, SPI_PAD_MASK, dir);
	local_irq_restore(flags);
}

#ifdef HG8245_SLICPROBE_UART
static int gpio4_set_and_check(void __iomem *gpio, u32 value)
{
	unsigned long flags;
	u32 latch, level;

	local_irq_save(flags);
	replace_bits(gpio + GPIO_DATA, GPIO4, value);
	latch = readl(gpio + GPIO_DATA) & GPIO4;
	local_irq_restore(flags);
	udelay(10);
	level = readl(gpio + GPIO_LEVEL) & GPIO4;
	if (latch != value || level != value) {
		pr_warn("GPIO4 latch/level check failed: wanted=%u latch=%u level=%u\n",
			!!value, !!latch, !!level);
		return -EIO;
	}
	return 0;
}

static int gpio4_trial_begin(struct regmap *crg, void __iomem *gpio, u32 dir)
{
	u32 pinmux;
	int ret;

	ret = regmap_read(crg, CRG_PINMUX, &pinmux);
	if (ret)
		return ret;
	if (pinmux != GPIO4_TRIAL_PINMUX || (dir & GPIO4) ||
	    !(readl(gpio + GPIO_OUTMODE) & GPIO4) ||
	    !(readl(gpio + GPIO_DATA) & GPIO4) ||
	    !(readl(gpio + GPIO_LEVEL) & GPIO4)) {
		pr_warn("GPIO4 trial guard: need the measured OpenWrt high-output baseline; no transfer\n");
		return -EAGAIN;
	}

	ret = gpio4_set_and_check(gpio, 0);
	if (ret) {
		if (gpio4_set_and_check(gpio, GPIO4))
			pr_err("could not verify restoration of GPIO4 high\n");
		return ret;
	}
	pr_info("GPIO4 trial: latch and level both low; running one stock ID probe\n");
	return 0;
}
#endif

static int tx_byte(void __iomem *spi, u8 byte)
{
	u32 status;
	int ret;

	program_spi_bits(spi + SPI_FORMAT, SPI_TX | SPI_RX, SPI_TX);
	replace_bits(spi + SPI_ENABLE, SPI_RUN, SPI_RUN);
	ret = readl_poll_timeout_atomic(spi + SPI_STATUS, status,
					status & SPI_TX_READY, 1, SPI_WAIT_US);
	if (!ret) {
		writel(byte, spi + SPI_DATA);
		udelay(5);
		ret = readl_poll_timeout_atomic(spi + SPI_STATUS, status,
						(status & SPI_IDLE_MASK) == SPI_IDLE,
						1, SPI_WAIT_US);
	}
	replace_bits(spi + SPI_ENABLE, SPI_RUN, 0);
	if (ret)
		pr_warn("TX byte %02x timed out; SPI status=%08x\n", byte, status);
	return ret;
}

static int configure_cs0(struct regmap *crg, void __iomem *spi,
			  void __iomem *gpio)
{
	u32 ctrl, status;
	int ret, restore;

	if (stock_ctrl_cycle) {
		/* hi_spi: clear only CRG+0x1c[21] before programming SPI. */
		ret = regmap_clear_bits(crg, CRG_CTRL, CRG_SPI_CTRL);
		if (ret)
			goto restore_ctrl;
		ret = regmap_read(crg, CRG_CTRL, &ctrl);
		if (ret || (ctrl & CRG_SPI_CTRL)) {
			ret = ret ?: -EIO;
			goto restore_ctrl;
		}
		ret = regmap_read(crg, CRG_STATUS, &status);
		if (ret || !(status & CRG_SPI)) {
			ret = ret ?: -EIO;
			goto restore_ctrl;
		}
	}

	replace_bits(spi + SPI_IRQCFG, SPI_IRQ_MASK, 0);
	set_pad_dir(gpio, SPI_PAD_CS0_DIR);
	program_spi_bits(spi + SPI_ENABLE, SPI_RUN, 0);
	program_spi_bits(spi + SPI_DIVISOR, SPI_DIVISOR_MASK, SPI_DIV_133MHZ);
	program_spi_bits(spi + SPI_FORMAT, SPI_FRAME_MASK, SPI_FRAME_8BIT);
	program_spi_bits(spi + SPI_SELECT, SPI_SELECT_MASK, BIT(0));
	ret = 0;
	if (!stock_ctrl_cycle)
		return ret;

restore_ctrl:
	restore = regmap_set_bits(crg, CRG_CTRL, CRG_SPI_CTRL);
	restore = restore ?: regmap_read(crg, CRG_CTRL, &ctrl);
	if (!restore && !(ctrl & CRG_SPI_CTRL))
		restore = -EIO;
	if (ret || restore) {
		pr_warn("controller transition failed: setup=%d restore=%d\n",
			ret, restore);
		return restore ?: ret;
	}
	if ((readl(spi + SPI_FORMAT) & SPI_FRAME_MASK) != SPI_FRAME_8BIT ||
	    (readl(spi + SPI_SELECT) & SPI_SELECT_MASK) != BIT(0) ||
	    (readl(spi + SPI_DIVISOR) & SPI_DIVISOR_MASK) != SPI_DIV_133MHZ) {
		pr_warn("controller setup did not survive the transition\n");
		return -EIO;
	}
	return 0;
}

#ifdef HG8245_SLICPROBE_UART
static int spi_gate_command(struct regmap *crg, bool on)
{
	u32 status;
	int ret;

	ret = regmap_write(crg, on ? CRG_CLK_SET : CRG_CLK_CLEAR, CRG_SPI);
	if (ret)
		return ret;
	return regmap_read_poll_timeout(crg, CRG_STATUS, status,
					!!(status & CRG_SPI) == on, 5, 1000);
}

static int cold_spi_begin(struct regmap *crg, void __iomem *spi,
			   void __iomem *gpio)
{
	u32 route;
	int ret, route_ret, clock_ret, ctrl_ret;

	/*
	 * The caller checked idle SPI and an initially enabled gate/route.
	 * Recreate the essential disable and first-enable branches in hi_spi.
	 * No SPI MMIO between the off command and verified clock recovery.
	 */
	ret = regmap_clear_bits(crg, CRG_CTRL, CRG_SPI_CTRL);
	ret = ret ?: regmap_clear_bits(crg, CRG_ROUTE, CRG_SPI_ROUTE);
	if (!ret) {
		set_pad_dir(gpio, 0);
		ret = spi_gate_command(crg, false);
	}
	if (!ret) {
		pr_info("cold SPI: clock off confirmed\n");
		set_pad_dir(gpio, BIT(3));
		ret = regmap_set_bits(crg, CRG_ROUTE, CRG_SPI_ROUTE);
		ret = ret ?: regmap_read(crg, CRG_ROUTE, &route);
		if (!ret && !(route & CRG_SPI_ROUTE))
			ret = -EIO;
	}
	if (!ret) {
		set_pad_dir(gpio, SPI_PAD_CS0_DIR);
		ret = spi_gate_command(crg, true);
	}
	if (!ret) {
		pr_info("cold SPI: clock on confirmed\n");
		ret = configure_cs0(crg, spi, gpio);
	}
	if (!ret)
		return 0;

	/* Restore the initial single bits even if one recovery step fails. */
	route_ret = regmap_set_bits(crg, CRG_ROUTE, CRG_SPI_ROUTE);
	clock_ret = spi_gate_command(crg, true);
	ctrl_ret = regmap_set_bits(crg, CRG_CTRL, CRG_SPI_CTRL);
	pr_warn("cold SPI aborted (%d); restore route=%d clock=%d ctrl=%d\n",
		ret, route_ret, clock_ret, ctrl_ret);
	return route_ret ?: clock_ret ?: ctrl_ret ?: ret;
}
#endif

static int write_mpi(struct regmap *crg, void __iomem *spi, void __iomem *gpio,
		     u8 command, const u8 *data, size_t len)
{
	size_t i;
	int ret = 0;

	if (stock_ctrl_cycle)
		ret = configure_cs0(crg, spi, gpio);
	if (!ret)
		ret = tx_byte(spi, command);
	for (i = 0; !ret && i < len; i++) {
		udelay(SLIC_WRITE_GAP_US);
		ret = tx_byte(spi, data[i]);
	}
	return ret;
}

static int stock_pre_id(struct regmap *crg, void __iomem *spi, void __iomem *gpio)
{
	static const u8 ec = 0x01;
	static const u8 sync[16] = { [0 ... 15] = SLIC_SYNC_COMMAND };
	int ret;

	/* Vp880Init before the first 0x73; no later device or line init. */
	ret = write_mpi(crg, spi, gpio, SLIC_EC_COMMAND, &ec, 1);
	if (!ret)
		ret = write_mpi(crg, spi, gpio, SLIC_SYNC_COMMAND, sync, sizeof(sync));
	if (!ret)
		ret = write_mpi(crg, spi, gpio, SLIC_EC_COMMAND, &ec, 1);
	return ret;
}

static int rx_id_byte(void __iomem *spi, u8 *byte)
{
	u32 status;
	int ret;

	udelay(50);
	program_spi_bits(spi + SPI_FORMAT, SPI_TX | SPI_RX, SPI_RX);
	replace_bits(spi + SPI_ENABLE, SPI_RUN, SPI_RUN);
	writel(0xffff, spi + SPI_DATA);
	ret = readl_poll_timeout_atomic(spi + SPI_STATUS, status,
					status & SPI_RX_READY, 1, SPI_WAIT_US);
	if (!ret)
		*byte = readl(spi + SPI_DATA) & 0xff;
	replace_bits(spi + SPI_ENABLE, SPI_RUN, 0);
	if (ret)
		pr_warn("ID read timed out; SPI status=%08x\n", status);
	return ret;
}

static int read_id(struct regmap *crg, void __iomem *spi, void __iomem *gpio,
		    u32 gpio_dir)
{
	u32 format, select, divisor, irqcfg, status;
	u8 id[2];
	int ret = 0;

	status = readl(spi + SPI_STATUS);
	if ((readl(spi + SPI_ENABLE) & SPI_RUN) ||
	    (status & (SPI_RX_READY | BIT(0)))) {
		pr_warn("controller not quiescent; SPI status=%08x, no transfer\n",
			status);
		return -EBUSY;
	}

	format = readl(spi + SPI_FORMAT);
	select = readl(spi + SPI_SELECT);
	divisor = readl(spi + SPI_DIVISOR);
	irqcfg = readl(spi + SPI_IRQCFG);
	pr_info("before CS0: format=%08x select=%08x divisor=%08x irq=%08x status=%08x\n",
		format, select, divisor, irqcfg, status);

#ifdef HG8245_SLICPROBE_UART
	if (gpio4_low_trial) {
		ret = gpio4_trial_begin(crg, gpio, gpio_dir);
		if (ret)
			return ret;
	}
	if (stock_cold_cycle)
		ret = cold_spi_begin(crg, spi, gpio);
#endif
	if (!ret && !stock_ctrl_cycle)
		ret = configure_cs0(crg, spi, gpio);

	if (!ret && !bare_id)
		ret = stock_pre_id(crg, spi, gpio);
	if (!ret)
		ret = write_mpi(crg, spi, gpio, SLIC_ID_COMMAND, NULL, 0);
	if (!ret)
		ret = rx_id_byte(spi, &id[0]);
	if (!ret)
		ret = rx_id_byte(spi, &id[1]);

	if (stock_ctrl_cycle && ret &&
	    (regmap_read(crg, CRG_STATUS, &status) || !(status & CRG_SPI))) {
		pr_err("SPI clock not confirmed after error; skipping SPI restore\n");
	} else {
		replace_bits(spi + SPI_FORMAT, SPI_FRAME_MASK | SPI_TX | SPI_RX, format);
		replace_bits(spi + SPI_IRQCFG, SPI_IRQ_MASK, irqcfg);
		replace_bits(spi + SPI_DIVISOR, SPI_DIVISOR_MASK, divisor);
		replace_bits(spi + SPI_SELECT, SPI_SELECT_MASK, select);
	}
	set_pad_dir(gpio, gpio_dir);

#ifdef HG8245_SLICPROBE_UART
	if (gpio4_low_trial) {
		if (gpio4_set_and_check(gpio, GPIO4)) {
			pr_err("could not verify restoration of GPIO4 high\n");
			return -EIO;
		}
		pr_info("GPIO4 trial: original high latch and level restored\n");
	}
#endif
	if (stock_ctrl_cycle)
		pr_info("stock controller transition probe finished (%d)\n", ret);
	if (ret)
		return ret;
	if (id[0] != 0x04 || id[1] != 0xb3) {
		pr_warn("CS0 preamble=%s ID=%02x %02x (stock diagnostic expects 04 b3)\n",
			bare_id ? "none" : "stock", id[0], id[1]);
		return -ENODEV;
	}
	pr_info("CS0 preamble=%s ID=%02x %02x matches the stock LE88226 diagnostic\n",
		bare_id ? "none" : "stock", id[0], id[1]);
	return 0;
}

static int match_gpio(struct gpio_chip *chip, void *data)
{
	return chip->parent && chip->parent->of_node == data;
}

#ifdef HG8245_SLICPROBE_UART
static void spi_snapshot(u32 ctrl, u32 status)
{
	void __iomem *spi;

	if (!(ctrl & CRG_SPI_CTRL) || !(status & CRG_SPI)) {
		pr_info("SPI clock/control not ready; skipping controller reads\n");
		return;
	}
	if (!request_mem_region(SPI_BASE, SPI_SIZE, KBUILD_MODNAME)) {
		pr_info("SPI MMIO region busy; skipping controller reads\n");
		return;
	}
	spi = ioremap(SPI_BASE, SPI_SIZE);
	if (spi) {
		/* Do not touch the FIFO or any interrupt-ack registers. */
		pr_info("SPI00=%08x SPI08=%08x SPI10=%08x SPI14=%08x SPI28=%08x SPI2c=%08x\n",
			readl(spi + SPI_FORMAT), readl(spi + SPI_ENABLE),
			readl(spi + SPI_SELECT), readl(spi + SPI_DIVISOR),
			readl(spi + SPI_STATUS), readl(spi + SPI_IRQCFG));
		iounmap(spi);
	} else {
		pr_warn("could not map the SPI controller for snapshot\n");
	}
	release_mem_region(SPI_BASE, SPI_SIZE);
}

static void pad_snapshot(struct regmap *crg, void __iomem *gpio)
{
	u32 parent, highway, pinmux;
	int ret;

	ret = regmap_read(crg, CRG_PARENT, &parent);
	ret = ret ?: regmap_read(crg, CRG_HIGHWAY, &highway);
	ret = ret ?: regmap_read(crg, CRG_PINMUX, &pinmux);
	if (ret)
		pr_warn("cannot read clock-source/pinmux context (%d)\n", ret);
	else
		pr_info("CRG18=%08x CRG34=%08x CRG54=%08x\n", parent, highway, pinmux);
	pr_info("GPIO0_DATA=%08x GPIO0_OUTMODE=%08x GPIO0_LEVEL=%08x GPIO2_DIR=%08x\n",
		readl(gpio + GPIO_DATA), readl(gpio + GPIO_OUTMODE),
		readl(gpio + GPIO_LEVEL), readl(gpio + GPIO2_DIR));
}
#endif

static int spi_probe(struct regmap *crg, void __iomem *gpio,
		     struct device_node *gpio_node)
{
	struct gpio_desc *pads[SPI_PAD_COUNT];
	struct gpio_chip *chip;
	void __iomem *spi;
	u32 ctrl, status, route, dir;
	int n = 0, ret;

	ret = regmap_read(crg, CRG_CTRL, &ctrl);
	ret = ret ?: regmap_read(crg, CRG_STATUS, &status);
	ret = ret ?: regmap_read(crg, CRG_ROUTE, &route);
	if (ret)
		return ret;

	dir = readl(gpio + GPIO_DIR);
	pr_info("CRG1c=%08x CRG2c=%08x CRG40=%08x GPIO0_DIR=%08x\n",
		ctrl, status, route, dir);
#ifdef HG8245_SLICPROBE_UART
	pad_snapshot(crg, gpio);
	if ((gpio4_low_trial || stock_cold_cycle) &&
	    (!tel_unplugged || !stock_ctrl_cycle || bare_id))
		return -EINVAL;
	if (gpio4_low_trial && stock_cold_cycle)
		return -EINVAL;
#endif
	if (!tel_unplugged) {
#ifdef HG8245_SLICPROBE_UART
		spi_snapshot(ctrl, status);
		pr_info("snapshot only; no MMIO writes or transfers\n");
#else
		pr_info("snapshot only; no SPI access\n");
#endif
		return 0;
	}
	if (!(ctrl & CRG_SPI_CTRL) || !(route & CRG_SPI_ROUTE) ||
	    !(status & CRG_SPI)) {
		pr_warn("clock/routing guard; no clock writes or SPI MMIO\n");
		return -EAGAIN;
	}

	chip = gpiochip_find(gpio_node, match_gpio);
	if (!chip)
		return -ENODEV;
	for (n = 0; n < SPI_PAD_COUNT; n++) {
		pads[n] = gpiochip_request_own_desc(chip, n, KBUILD_MODNAME,
						    GPIO_LOOKUP_FLAGS_DEFAULT,
						    GPIOD_ASIS);
		if (IS_ERR(pads[n])) {
			ret = PTR_ERR(pads[n]);
			pr_warn("cannot reserve GPIO0 pad %d (%d); no transfer\n",
				n, ret);
			goto free_pads;
		}
	}
	if (!request_mem_region(SPI_BASE, SPI_SIZE, KBUILD_MODNAME)) {
		ret = -EBUSY;
		goto free_pads;
	}
	spi = ioremap(SPI_BASE, SPI_SIZE);
	if (!spi) {
		ret = -ENOMEM;
		goto free_region;
	}

	/* Gate set/clear are write-one commands, not RMW registers. */
	ret = regmap_read(crg, CRG_STATUS, &status);
	if (!ret && !(status & CRG_SPI))
		ret = -EAGAIN;
	if (!ret)
		ret = read_id(crg, spi, gpio, readl(gpio + GPIO_DIR));
	iounmap(spi);
free_region:
	release_mem_region(SPI_BASE, SPI_SIZE);
free_pads:
	while (n)
		gpiochip_free_own_desc(pads[--n]);
	return ret;
}

#ifdef HG8245_SLICPROBE_EMBEDDED
int hg8245_codec_read_id(void)
#else
static int __init slicprobe_init(void)
#endif
{
	struct device_node *node, *gpio_node;
	struct resource res;
	struct regmap *crg;
	void __iomem *gpio;
	int ret;

	if (!of_machine_is_compatible("huawei,hg8245") ||
	    !of_machine_is_compatible("hisilicon,sd5113"))
		return -ENODEV;
	node = of_find_compatible_node(NULL, NULL, "hisilicon,sd5113-spi");
	if (!node)
		return -ENODEV;
	ret = of_address_to_resource(node, 0, &res);
	if (!ret && (res.start != SPI_BASE || resource_size(&res) != SPI_SIZE ||
		     of_device_is_available(node)))
		ret = -ENODEV;
	of_node_put(node);
	if (ret)
		return ret;

	crg = syscon_regmap_lookup_by_compatible("hisilicon,sd5113-crg");
	if (IS_ERR(crg))
		return PTR_ERR(crg);
	gpio_node = of_find_compatible_node(NULL, NULL, "hisilicon,sd5113-gpio");
	if (!gpio_node)
		return -ENODEV;
	ret = of_address_to_resource(gpio_node, 0, &res);
	if (ret)
		goto put_gpio_node;
	if (res.start != GPIO_BASE || resource_size(&res) < GPIO_SNAPSHOT_SIZE) {
		ret = -ENODEV;
		goto put_gpio_node;
	}
	gpio = ioremap(res.start, GPIO_SNAPSHOT_SIZE);
	if (!gpio) {
		ret = -ENOMEM;
		goto put_gpio_node;
	}
	ret = spi_probe(crg, gpio, gpio_node);
	iounmap(gpio);
put_gpio_node:
	of_node_put(gpio_node);
	return ret;
}

#ifndef HG8245_SLICPROBE_EMBEDDED
static void __exit slicprobe_exit(void)
{
}

module_init(slicprobe_init);
module_exit(slicprobe_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("One-shot HG8245 LE88226 SPI identification probe");
#endif
