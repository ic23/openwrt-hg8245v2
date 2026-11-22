// SPDX-License-Identifier: GPL-2.0
/* HiSilicon SD5113 GPIO controller; interrupt routing is not implemented. */

#include <linux/debugfs.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/sd5113.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>

#define SD5113_GPIO_NPINS	64
#define SD5113_GPIO_PORT_SHIFT	0x1000
#define SD5113_GPIO_PINS_PER_PORT 8

#define SD5113_GPIO_DATA	0x00
#define SD5113_GPIO_DIR		0x04
#define SD5113_GPIO_FUNC	0x08
#define SD5113_GPIO_LEVEL	0x50

/* CRG+0x54 bit selections made by stock hi_kernel_gpio_bit_attr_set(). */
struct sd5113_pinmux_entry {
	u8 pin;
	u8 bit;
};

static const struct sd5113_pinmux_entry sd5113_pinmux_table[] = {
	{ 11, 28 },
	{ 44, 0 }, { 45, 1 }, { 46, 3 }, { 47, 4 },
	{ 48, 5 }, { 49, 6 }, { 50, 7 }, { 51, 8 },
	{ 52, 9 }, { 53, 10 }, { 54, 11 }, { 55, 12 },
	{ 56, 13 }, { 57, 14 }, { 58, 15 }, { 59, 16 },
	{ 60, 17 }, { 61, 18 }, { 62, 26 }, { 63, 27 },
};

struct sd5113_gpio {
	struct gpio_chip chip;
	void __iomem *base;
	struct regmap *crg;
	spinlock_t lock;
};

#define to_sd5113_gpio(gc) container_of(gc, struct sd5113_gpio, chip)

static int sd5113_pinmux_bit(unsigned int pin)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(sd5113_pinmux_table); i++) {
		if (sd5113_pinmux_table[i].pin == pin)
			return sd5113_pinmux_table[i].bit;
	}

	return -1;
}

static int sd5113_gpio_request(struct gpio_chip *chip, unsigned int offset)
{
	struct sd5113_gpio *gpio = to_sd5113_gpio(chip);
	int bit;

	bit = sd5113_pinmux_bit(offset);
	if (bit < 0)
		return 0;

	return regmap_set_bits(gpio->crg, 0x54, BIT(bit));
}

static void __iomem *sd5113_gpio_port_base(struct sd5113_gpio *gpio,
					    unsigned int pin)
{
	unsigned int port = pin / SD5113_GPIO_PINS_PER_PORT;

	return gpio->base + (port * SD5113_GPIO_PORT_SHIFT);
}

static unsigned int sd5113_gpio_bit(unsigned int pin)
{
	return pin % SD5113_GPIO_PINS_PER_PORT;
}

static int sd5113_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	struct sd5113_gpio *gpio = to_sd5113_gpio(chip);
	void __iomem *port = sd5113_gpio_port_base(gpio, offset);
	u32 val = readl(port + SD5113_GPIO_LEVEL);

	return !!(val & BIT(sd5113_gpio_bit(offset)));
}

static void sd5113_gpio_update(void __iomem *reg, u32 mask, u32 value)
{
	u32 old = readl(reg);

	writel((old & ~mask) | (value & mask), reg);
}

static void sd5113_gpio_set(struct gpio_chip *chip, unsigned int offset, int value)
{
	struct sd5113_gpio *gpio = to_sd5113_gpio(chip);
	void __iomem *port = sd5113_gpio_port_base(gpio, offset);
	u32 mask = BIT(sd5113_gpio_bit(offset));
	unsigned long flags;

	spin_lock_irqsave(&gpio->lock, flags);
	sd5113_gpio_update(port + SD5113_GPIO_DATA, mask, value ? mask : 0);
	spin_unlock_irqrestore(&gpio->lock, flags);
}

static int sd5113_gpio_direction_input(struct gpio_chip *chip,
					unsigned int offset)
{
	struct sd5113_gpio *gpio = to_sd5113_gpio(chip);
	void __iomem *port = sd5113_gpio_port_base(gpio, offset);
	u32 mask = BIT(sd5113_gpio_bit(offset));
	unsigned long flags;

	spin_lock_irqsave(&gpio->lock, flags);
	sd5113_gpio_update(port + SD5113_GPIO_DIR, mask, 0);
	sd5113_gpio_update(port + SD5113_GPIO_FUNC, mask, 0);
	spin_unlock_irqrestore(&gpio->lock, flags);

	return 0;
}

static int sd5113_gpio_direction_output(struct gpio_chip *chip,
					 unsigned int offset, int value)
{
	struct sd5113_gpio *gpio = to_sd5113_gpio(chip);
	void __iomem *port = sd5113_gpio_port_base(gpio, offset);
	u32 mask = BIT(sd5113_gpio_bit(offset));
	unsigned long flags;

	spin_lock_irqsave(&gpio->lock, flags);
	/* Load the latch before enabling the GPIO output driver. */
	sd5113_gpio_update(port + SD5113_GPIO_DATA, mask, value ? mask : 0);
	sd5113_gpio_update(port + SD5113_GPIO_DIR, mask, mask);
	sd5113_gpio_update(port + SD5113_GPIO_FUNC, mask, 0);
	spin_unlock_irqrestore(&gpio->lock, flags);

	return 0;
}

static int sd5113_gpio_get_direction(struct gpio_chip *chip,
				      unsigned int offset)
{
	struct sd5113_gpio *gpio = to_sd5113_gpio(chip);
	void __iomem *port = sd5113_gpio_port_base(gpio, offset);
	u32 val = readl(port + SD5113_GPIO_DIR);

	return (val & BIT(sd5113_gpio_bit(offset))) ?
		GPIO_LINE_DIRECTION_OUT : GPIO_LINE_DIRECTION_IN;
}

int sd5113_gpio_set_funcs(struct gpio_desc * const *pads, unsigned int count,
			 u32 mask, u32 value, u32 *old)
{
	struct gpio_chip *chip;
	struct sd5113_gpio *gpio;
	void __iomem *port;
	unsigned long flags;
	int bank = -1;
	unsigned int i;
	u32 funcs, owned = 0;

	if (!pads || !count || count > SD5113_GPIO_PINS_PER_PORT || !pads[0])
		return -EINVAL;
	chip = gpiod_to_chip(pads[0]);
	if (!chip || chip->request != sd5113_gpio_request)
		return -EINVAL;
	for (i = 0; i < count; i++) {
		int pin, port_index;
		u32 bit;

		if (!pads[i] || gpiod_to_chip(pads[i]) != chip)
			return -EINVAL;
		pin = desc_to_gpio(pads[i]) - chip->base;
		if (pin < 0 || pin >= SD5113_GPIO_NPINS)
			return -EINVAL;
		port_index = pin / SD5113_GPIO_PINS_PER_PORT;
		bit = BIT(sd5113_gpio_bit(pin));
		if ((bank >= 0 && bank != port_index) || (owned & bit))
			return -EINVAL;
		bank = port_index;
		owned |= bit;
	}
	if (mask & ~owned)
		return -EINVAL;

	gpio = to_sd5113_gpio(chip);
	port = gpio->base + bank * SD5113_GPIO_PORT_SHIFT;
	spin_lock_irqsave(&gpio->lock, flags);
	funcs = readl(port + SD5113_GPIO_FUNC);
	if (old)
		*old = funcs & owned;
	if (mask)
		writel((funcs & ~mask) | (value & mask), port + SD5113_GPIO_FUNC);
	spin_unlock_irqrestore(&gpio->lock, flags);
	return 0;
}
EXPORT_SYMBOL_GPL(sd5113_gpio_set_funcs);

int sd5113_gpio_read_bank_funcs(struct gpio_desc *pad, u32 *funcs)
{
	struct gpio_chip *chip;
	struct sd5113_gpio *gpio;
	void __iomem *port;
	unsigned long flags;
	int pin;

	if (!pad || !funcs)
		return -EINVAL;
	chip = gpiod_to_chip(pad);
	if (!chip || chip->request != sd5113_gpio_request)
		return -EINVAL;
	pin = desc_to_gpio(pad) - chip->base;
	if (pin < 0 || pin >= SD5113_GPIO_NPINS)
		return -EINVAL;

	gpio = to_sd5113_gpio(chip);
	port = sd5113_gpio_port_base(gpio, pin);
	spin_lock_irqsave(&gpio->lock, flags);
	*funcs = readl(port + SD5113_GPIO_FUNC) & (BIT(SD5113_GPIO_PINS_PER_PORT) - 1);
	spin_unlock_irqrestore(&gpio->lock, flags);
	return 0;
}
EXPORT_SYMBOL_GPL(sd5113_gpio_read_bank_funcs);

struct sd5113_gpio_bank_state {
	u32 data, dir, func, level;
};

static void sd5113_gpio_read_bank(struct sd5113_gpio *gpio, unsigned int bank,
				  struct sd5113_gpio_bank_state *state)
{
	void __iomem *port = sd5113_gpio_port_base(gpio, bank * SD5113_GPIO_PINS_PER_PORT);
	unsigned long flags;

	spin_lock_irqsave(&gpio->lock, flags);
	state->data = readl(port + SD5113_GPIO_DATA);
	state->dir = readl(port + SD5113_GPIO_DIR);
	state->func = readl(port + SD5113_GPIO_FUNC);
	state->level = readl(port + SD5113_GPIO_LEVEL);
	spin_unlock_irqrestore(&gpio->lock, flags);
}

static int sd5113_gpio_banks_show(struct seq_file *s, void *unused)
{
	struct sd5113_gpio *gpio = s->private;
	unsigned int bank;
	u32 mux;
	int ret;

	ret = regmap_read(gpio->crg, 0x54, &mux);
	if (ret)
		return ret;
	seq_printf(s, "CRG54=%08x\n", mux);
	seq_puts(s, "BANK DATA     DIR      FUNC     LEVEL\n");
	for (bank = 0; bank < SD5113_GPIO_NPINS / SD5113_GPIO_PINS_PER_PORT; bank++) {
		struct sd5113_gpio_bank_state state;

		sd5113_gpio_read_bank(gpio, bank, &state);
		seq_printf(s, "%4u %08x %08x %08x %08x\n", bank, state.data,
			   state.dir, state.func, state.level);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(sd5113_gpio_banks);

static void sd5113_gpio_remove_debugfs(void *entry)
{
	debugfs_remove(entry);
}

static int sd5113_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sd5113_gpio *gpio;
	int ret;

	gpio = devm_kzalloc(dev, sizeof(*gpio), GFP_KERNEL);
	if (!gpio)
		return -ENOMEM;

	gpio->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(gpio->base))
		return PTR_ERR(gpio->base);

	gpio->crg = syscon_regmap_lookup_by_phandle(dev->of_node, "hisilicon,crg");
	if (IS_ERR(gpio->crg))
		return dev_err_probe(dev, PTR_ERR(gpio->crg), "cannot get CRG\n");

	spin_lock_init(&gpio->lock);

	gpio->chip.label = "sd5113-gpio";
	gpio->chip.parent = dev;
	gpio->chip.owner = THIS_MODULE;
	gpio->chip.base = -1;
	gpio->chip.ngpio = SD5113_GPIO_NPINS;
	gpio->chip.request = sd5113_gpio_request;
	gpio->chip.get = sd5113_gpio_get;
	gpio->chip.set = sd5113_gpio_set;
	gpio->chip.direction_input = sd5113_gpio_direction_input;
	gpio->chip.direction_output = sd5113_gpio_direction_output;
	gpio->chip.get_direction = sd5113_gpio_get_direction;
	gpio->chip.can_sleep = false;

	ret = devm_gpiochip_add_data(dev, &gpio->chip, gpio);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, gpio);

	if (of_property_read_bool(pdev->dev.of_node, "hisilicon,debug-gpio-banks")) {
		struct dentry *entry;

		entry = debugfs_create_file("sd5113-gpio-banks", 0400, NULL, gpio,
					    &sd5113_gpio_banks_fops);
		if (!IS_ERR_OR_NULL(entry) &&
		    devm_add_action_or_reset(&pdev->dev, sd5113_gpio_remove_debugfs, entry))
			dev_warn(&pdev->dev, "could not keep GPIO bank snapshot\n");
	}
	return 0;
}

static const struct of_device_id sd5113_gpio_of_match[] = {
	{ .compatible = "hisilicon,sd5113-gpio" },
	{ }
};
MODULE_DEVICE_TABLE(of, sd5113_gpio_of_match);

static struct platform_driver sd5113_gpio_driver = {
	.probe = sd5113_gpio_probe,
	.driver = {
		.name = "sd5113-gpio",
		.of_match_table = sd5113_gpio_of_match,
	},
};
module_platform_driver(sd5113_gpio_driver);

MODULE_DESCRIPTION("HiSilicon SD5113 GPIO driver");
MODULE_LICENSE("GPL");
