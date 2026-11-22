// SPDX-License-Identifier: GPL-2.0
/* HiSilicon SD5113 (Huawei HG8245) GPIO controller */

#include <linux/gpio/driver.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#define SD5113_GPIO_NPINS	64
#define SD5113_GPIO_PORT_SHIFT	0x1000
#define SD5113_GPIO_PINS_PER_PORT 8

#define SD5113_GPIO_DATA	0x00
#define SD5113_GPIO_DIR		0x08
#define SD5113_GPIO_LEVEL	0x50

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
	void __iomem *pinmux; /* CRG+0x54, physical 0x16140054 - may be NULL
				* if the DTS does not provide a second reg
				* entry, in which case pinmux is skipped
				* entirely (see sd5113_gpio_request()) */
	spinlock_t lock; /* protects read-modify-write of DATA/DIR/pinmux */
};

#define to_sd5113_gpio(gc) container_of(gc, struct sd5113_gpio, chip)

static int sd5113_pinmux_bit(unsigned int pin)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(sd5113_pinmux_table); i++) {
		if (sd5113_pinmux_table[i].pin == pin)
			return sd5113_pinmux_table[i].bit;
	}

	return -1; /* pin needs no pinmux write - CONFIRMED, not a gap */
}

static int sd5113_gpio_request(struct gpio_chip *chip, unsigned int offset)
{
	struct sd5113_gpio *gpio = to_sd5113_gpio(chip);
	unsigned long flags;
	int bit;
	u32 val;

	bit = sd5113_pinmux_bit(offset);
	if (bit < 0)
		return 0; /* nothing to do for this pin - see table comment */

	if (!gpio->pinmux) {
		dev_warn_once(chip->parent,
			      "pin %u needs pinmux bit %d but no pinmux reg was provided in the DTS - GPIO function may not actually reach the pin\n",
			      offset, bit);
		return 0;
	}

	spin_lock_irqsave(&gpio->lock, flags);
	val = readl(gpio->pinmux);
	val |= BIT(bit);
	writel(val, gpio->pinmux);
	spin_unlock_irqrestore(&gpio->lock, flags);

	return 0;
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

static void sd5113_gpio_set(struct gpio_chip *chip, unsigned int offset,
			     int value)
{
	struct sd5113_gpio *gpio = to_sd5113_gpio(chip);
	void __iomem *port = sd5113_gpio_port_base(gpio, offset);
	unsigned int bit = sd5113_gpio_bit(offset);
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&gpio->lock, flags);
	val = readl(port + SD5113_GPIO_DATA);
	if (value)
		val |= BIT(bit);
	else
		val &= ~BIT(bit);
	writel(val, port + SD5113_GPIO_DATA);
	spin_unlock_irqrestore(&gpio->lock, flags);
}

static int sd5113_gpio_direction_input(struct gpio_chip *chip,
					unsigned int offset)
{
	struct sd5113_gpio *gpio = to_sd5113_gpio(chip);
	void __iomem *port = sd5113_gpio_port_base(gpio, offset);
	unsigned int bit = sd5113_gpio_bit(offset);
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&gpio->lock, flags);
	val = readl(port + SD5113_GPIO_DIR);
	val |= BIT(bit); /* CONFIRMED: 1 = input */
	writel(val, port + SD5113_GPIO_DIR);
	spin_unlock_irqrestore(&gpio->lock, flags);

	return 0;
}

static int sd5113_gpio_direction_output(struct gpio_chip *chip,
					 unsigned int offset, int value)
{
	struct sd5113_gpio *gpio = to_sd5113_gpio(chip);
	void __iomem *port = sd5113_gpio_port_base(gpio, offset);
	unsigned int bit = sd5113_gpio_bit(offset);
	unsigned long flags;
	u32 val;

	/*
	 * CONFIRMED order (matches u-boot's own sequence): set the output
	 * level FIRST, then switch direction to output - this avoids a
	 * glitch where the pin would briefly drive whatever the DATA
	 * register already happened to hold before we set our intended
	 * value.
	 */
	sd5113_gpio_set(chip, offset, value);

	spin_lock_irqsave(&gpio->lock, flags);
	val = readl(port + SD5113_GPIO_DIR);
	val &= ~BIT(bit); /* CONFIRMED: 0 = output */
	writel(val, port + SD5113_GPIO_DIR);
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
		GPIO_LINE_DIRECTION_IN : GPIO_LINE_DIRECTION_OUT;
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

	if (platform_get_resource(pdev, IORESOURCE_MEM, 1)) {
		gpio->pinmux = devm_platform_ioremap_resource(pdev, 1);
		if (IS_ERR(gpio->pinmux))
			return PTR_ERR(gpio->pinmux);
	}

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
