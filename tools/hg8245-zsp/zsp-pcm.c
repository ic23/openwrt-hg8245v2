// SPDX-License-Identifier: GPL-2.0-only
#define pr_fmt(fmt) "hg8245-zsp-pcm: " fmt

#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/machine.h>
#include <linux/gpio/sd5113.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/regmap.h>

#include "zsp-pcm.h"

#define GPIO_PHYS		0x16146000
#define GPIO2_FUNC		0x2008
#define PCM_GPIO_FIRST		18
#define PCM_GPIO_MASK		GENMASK(5, 2)
#define CRG_CONTROL		0x1c
#define CRG_CLOCK_MODE		0x34
#define CRG_HIGHWAY_ENABLE	0x38
#define PCM_CONTROL_BIT		BIT(11)
#define PCM_CLOCK_MASK		(BIT(7) | BIT(0))
#define PCM_PCLK_MASK		GENMASK(6, 4)
#define PCM_ENABLE_BIT		BIT(27)
#define PCM_HW_PCLK		0x04
#define PCM_HW_WIDTH		0x08
#define PCM_HW_WIDTH_BIT	BIT(0)
#define PCM_HW_MASK		0x88
#define PCM_HW_CHAN_MASK	GENMASK(3, 0)

static int match_gpio(struct gpio_chip *chip, void *data)
{
	return chip->parent && chip->parent->of_node == data;
}

static void pcm_update(void __iomem *reg, u32 mask, u32 bits)
{
	u32 old = readl(reg);
	u32 new = (old & ~mask) | (bits & mask);

	if (old != new)
		writel(new, reg);
}

static int pcm_funcs(struct hg8245_pcm *pcm, u32 mask, u32 bits, u32 *old)
{
	return sd5113_gpio_set_funcs(pcm->pads, ARRAY_SIZE(pcm->pads), mask, bits, old);
}

static int pcm_fields(struct hg8245_pcm *pcm, u32 width, u32 channels)
{
	u32 control;
	int ret, restore;

	if ((readl(pcm->hw + PCM_HW_WIDTH) & PCM_HW_WIDTH_BIT) ==
	    (width & PCM_HW_WIDTH_BIT) &&
	    (readl(pcm->hw + PCM_HW_MASK) & PCM_HW_CHAN_MASK) ==
	    (channels & PCM_HW_CHAN_MASK)) {
		/* Also repair a lost configuration bit on the error path. */
		ret = regmap_set_bits(pcm->crg, CRG_CONTROL, PCM_CONTROL_BIT);
		ret = ret ?: regmap_read(pcm->crg, CRG_CONTROL, &control);
		return ret ?: ((control & PCM_CONTROL_BIT) ? 0 : -EIO);
	}

	/* hi_hw attr_set changes format with CRG+0x1c[11] clear. */
	ret = regmap_clear_bits(pcm->crg, CRG_CONTROL, PCM_CONTROL_BIT);
	ret = ret ?: regmap_read(pcm->crg, CRG_CONTROL, &control);
	if (!ret && (control & PCM_CONTROL_BIT))
		ret = -EIO;
	if (!ret) {
		pcm_update(pcm->hw + PCM_HW_WIDTH, PCM_HW_WIDTH_BIT, width);
		pcm_update(pcm->hw + PCM_HW_MASK, PCM_HW_CHAN_MASK, channels);
	}
	restore = regmap_set_bits(pcm->crg, CRG_CONTROL, PCM_CONTROL_BIT);
	restore = restore ?: regmap_read(pcm->crg, CRG_CONTROL, &control);
	if (restore || !(control & PCM_CONTROL_BIT)) {
		pr_err("PCM configuration bit did not recover\n");
		return restore ?: -EIO;
	}
	if (ret)
		return ret;
	if (((readl(pcm->hw + PCM_HW_WIDTH) ^ width) & PCM_HW_WIDTH_BIT) ||
	    ((readl(pcm->hw + PCM_HW_MASK) ^ channels) & PCM_HW_CHAN_MASK))
		return -EIO;
	return 0;
}

static int pcm_chain_fields(struct hg8245_pcm *pcm, u32 width, u32 channels,
			    u32 clock, u32 enable)
{
	u32 control, clk, enabled;
	int ret, recovery;

	ret = regmap_clear_bits(pcm->crg, CRG_CONTROL, PCM_CONTROL_BIT);
	ret = ret ?: regmap_read(pcm->crg, CRG_CONTROL, &control);
	if (!ret && (control & PCM_CONTROL_BIT))
		ret = -EIO;
	ret = ret ?: regmap_update_bits(pcm->crg, CRG_CLOCK_MODE, PCM_PCLK_MASK, clock);
	if (!ret) {
		pcm_update(pcm->hw + PCM_HW_WIDTH, PCM_HW_WIDTH_BIT, width);
		pcm_update(pcm->hw + PCM_HW_MASK, PCM_HW_CHAN_MASK, channels);
		ret = regmap_update_bits(pcm->crg, CRG_HIGHWAY_ENABLE, PCM_ENABLE_BIT, enable);
	}
	recovery = regmap_set_bits(pcm->crg, CRG_CONTROL, PCM_CONTROL_BIT);
	recovery = recovery ?: regmap_read(pcm->crg, CRG_CONTROL, &control);
	if (recovery || !(control & PCM_CONTROL_BIT))
		return recovery ?: -EIO;
	ret = ret ?: regmap_read(pcm->crg, CRG_CLOCK_MODE, &clk);
	ret = ret ?: regmap_read(pcm->crg, CRG_HIGHWAY_ENABLE, &enabled);
	if (ret)
		return ret;
	if (((clk ^ clock) & PCM_PCLK_MASK) || ((enabled ^ enable) & PCM_ENABLE_BIT) ||
	    ((readl(pcm->hw + PCM_HW_WIDTH) ^ width) & PCM_HW_WIDTH_BIT) ||
	    ((readl(pcm->hw + PCM_HW_MASK) ^ channels) & PCM_HW_CHAN_MASK))
		return -EIO;
	return 0;
}

int hg8245_pcm_finish(struct hg8245_pcm *pcm, bool restore)
{
	unsigned int i;
	u32 funcs;
	int ret = 0, pinret;

	if (pcm->armed && restore) {
		if (pcm->chain) {
			ret = regmap_clear_bits(pcm->crg, CRG_HIGHWAY_ENABLE, PCM_ENABLE_BIT);
			ret = pcm_chain_fields(pcm, pcm->old_width, pcm->old_mask,
					       pcm->old_clock, pcm->old_enable) ?: ret;
		} else {
			ret = pcm_fields(pcm, pcm->old_width, pcm->old_mask);
		}
		pinret = pcm_funcs(pcm, PCM_GPIO_MASK, pcm->old_funcs, NULL);
		pinret = pinret ?: pcm_funcs(pcm, 0, 0, &funcs);
		ret = ret ?: pinret;
		if (!pinret && funcs != pcm->old_funcs)
			ret = ret ?: -EIO;
		if (ret)
			pr_err("PCM/GPIO setup restoration failed (%d)\n", ret);
		else
			pr_info("PCM/GPIO masked fields restored\n");
	}
	pcm->armed = false;
	for (i = 0; i < ARRAY_SIZE(pcm->pads); i++) {
		if (pcm->pads[i]) {
			gpiochip_free_own_desc(pcm->pads[i]);
			pcm->pads[i] = NULL;
		}
	}
	return ret;
}

int hg8245_pcm_prepare(struct hg8245_pcm *pcm, struct regmap *crg, void __iomem *hw,
		       bool chain)
{
	struct device_node *node;
	struct gpio_chip *chip;
	struct resource res;
	u32 clock_mode, control, funcs, pclk = 0;
	unsigned int i;
	int ret, restore;

	pcm->crg = crg;
	pcm->hw = hw;
	ret = regmap_read(crg, CRG_CLOCK_MODE, &clock_mode);
	ret = ret ?: regmap_read(crg, CRG_CONTROL, &control);
	if (ret)
		return ret;
	if ((clock_mode & PCM_CLOCK_MASK) != BIT(0) ||
	    !(control & PCM_CONTROL_BIT)) {
		pr_warn("unexpected PCM baseline: CRG34=%08x CRG1c=%08x\n",
			clock_mode, control);
		return -EAGAIN;
	}
	if (chain) {
		ret = regmap_read(crg, CRG_HIGHWAY_ENABLE, &pcm->old_enable);
		if (ret)
			return ret;
		pclk = readl(hw + PCM_HW_PCLK) & 7;
		if (pclk > 4)
			return -EINVAL;
		pcm->old_clock = clock_mode;
		pcm->chain = true;
	}

	node = of_find_compatible_node(NULL, NULL, "hisilicon,sd5113-gpio");
	if (!node)
		return -ENODEV;
	ret = of_address_to_resource(node, 0, &res);
	chip = gpiochip_find(node, match_gpio);
	of_node_put(node);
	if (ret || !chip || res.start != GPIO_PHYS ||
	    resource_size(&res) < GPIO2_FUNC + 4)
		return ret ?: -ENODEV;

	for (i = 0; i < ARRAY_SIZE(pcm->pads); i++) {
		struct gpio_desc *pad;

		pad = gpiochip_request_own_desc(chip, PCM_GPIO_FIRST + i, KBUILD_MODNAME,
					      GPIO_LOOKUP_FLAGS_DEFAULT, GPIOD_ASIS);
		if (IS_ERR(pad)) {
			ret = PTR_ERR(pad);
			goto abort;
		}
		pcm->pads[i] = pad;
	}

	ret = pcm_funcs(pcm, 0, 0, &pcm->old_funcs);
	if (ret)
		goto abort;
	pcm->old_width = readl(hw + PCM_HW_WIDTH);
	pcm->old_mask = readl(hw + PCM_HW_MASK);
	pr_info("before: GPIO2_FUNC[5:2] masked=%02x HW00=%08x HW04=%08x HW08=%08x HW88=%08x\n",
		pcm->old_funcs, readl(hw), readl(hw + 4), pcm->old_width, pcm->old_mask);
	pcm->armed = true;
	if (chain) {
		ret = regmap_clear_bits(crg, CRG_HIGHWAY_ENABLE, PCM_ENABLE_BIT);
		ret = ret ?: regmap_read(crg, CRG_HIGHWAY_ENABLE, &control);
		if (ret || (control & PCM_ENABLE_BIT)) {
			ret = ret ?: -EIO;
			goto abort;
		}
	}
	ret = pcm_funcs(pcm, PCM_GPIO_MASK, PCM_GPIO_MASK, NULL);
	if (!ret)
		ret = chain ? pcm_chain_fields(pcm, PCM_HW_WIDTH_BIT, 0, pclk << 4,
					     PCM_ENABLE_BIT) : pcm_fields(pcm, PCM_HW_WIDTH_BIT, 0);
	ret = ret ?: pcm_funcs(pcm, 0, 0, &funcs);
	if (!ret && funcs != PCM_GPIO_MASK)
		ret = -EIO;
	if (ret)
		goto abort;
	pr_info("PCM host fields verified: GPIO2_FUNC[5:2] masked=%02x HW08=%08x HW88=%08x\n",
		funcs, readl(hw + PCM_HW_WIDTH), readl(hw + PCM_HW_MASK));
	if (chain)
		pr_info("HIGHWAY CRG+0x38[27] clear/set verified, inherited PCLK selector=%u\n", pclk);
	return 0;

abort:
	restore = hg8245_pcm_finish(pcm, true);
	return restore ?: ret;
}
