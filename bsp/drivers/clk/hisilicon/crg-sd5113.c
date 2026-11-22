// SPDX-License-Identifier: GPL-2.0
/* HiSilicon SD5113 (Huawei HG8245) Clock & Reset Generator (CRG) */

#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <dt-bindings/clock/sd5113-crg.h>

#define SD5113_CRG_SRC_SEL	0x18
#define SD5113_CRG_MODE		0x48
#define SD5113_CRG_PLL_LO	0x60
#define SD5113_CRG_PLL_HI	0x64

/*
 * CONFIRMED per-peripheral gate register offsets and bit INDEXES
 * (round 14; not masks, since clk_hw_register_gate() below wants a
 * bit index, not a pre-shifted mask - see file header for how each
 * was found).
 */
#define SD5113_CRG_GATE0		0x00	/* bit18 = unconfirmed (UART?) */
#define SD5113_CRG_GATE0_TIMER_BIT1	20	/* ARM SP804 timer, set 1st */
#define SD5113_CRG_GATE0_TIMER_BIT2	22	/* ARM SP804 timer, set 2nd */
#define SD5113_CRG_GATE_1C		0x1C	/* shared, multi-peripheral */
#define SD5113_CRG_GATE_1C_SPI		26
#define SD5113_CRG_GATE_1C_PCI_RESET	27	/* PCI reset strobe, round 13 */
#define SD5113_CRG_GATE_28		0x28	/* new in round 14 */
#define SD5113_CRG_GATE_28_I2C		27

struct sd5113_crg {
	void __iomem *base;
	spinlock_t lock; /* shared by every clk_hw_register_gate() call below -
			  * protects read-modify-write of shared gate regs
			  * (+0x1C is SPI's bit here AND PCI's reset-strobe
			  * bit elsewhere in the same register). */
	struct clk_hw_onecell_data *clk_data;
};

/*
 * Custom clk_hw ONLY for the one thing that genuinely needs it: reading
 * back the PLL-derived AHB rate. Every gated clock in this file uses
 * the stock clk_hw_register_gate() helper instead (see probe()) - no
 * other custom clk_ops in this file.
 */
struct sd5113_ahb_clk {
	struct clk_hw hw;
	void __iomem *base;
};

#define to_sd5113_ahb(_hw) container_of(_hw, struct sd5113_ahb_clk, hw)

#define SD5113_AHB_RATE_HZ	133250000UL

static unsigned long sd5113_ahb_recalc_rate(struct clk_hw *hw,
					     unsigned long parent_rate)
{
	return SD5113_AHB_RATE_HZ;
}

static const struct clk_ops sd5113_ahb_ops = {
	.recalc_rate = sd5113_ahb_recalc_rate,
};

static int sd5113_crg_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sd5113_crg *crg;
	struct sd5113_ahb_clk *ahb;
	struct clk_init_data ahb_init = {};
	struct clk_hw *hw, *timer_gate1_hw;
	int ret;

	crg = devm_kzalloc(dev, sizeof(*crg), GFP_KERNEL);
	if (!crg)
		return -ENOMEM;

	crg->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(crg->base))
		return PTR_ERR(crg->base);

	spin_lock_init(&crg->lock);

	crg->clk_data = devm_kzalloc(dev, struct_size(crg->clk_data, hws,
						       SD5113_CLK_NUM_CLOCKS),
				      GFP_KERNEL);
	if (!crg->clk_data)
		return -ENOMEM;
	crg->clk_data->num = SD5113_CLK_NUM_CLOCKS;

	/* ahb_clk: custom rate-only clk_hw, no gate (see file header). */
	ahb = devm_kzalloc(dev, sizeof(*ahb), GFP_KERNEL);
	if (!ahb)
		return -ENOMEM;
	ahb_init.name = "ahb_clk";
	ahb_init.ops = &sd5113_ahb_ops;
	ahb->hw.init = &ahb_init;
	ahb->base = crg->base;
	ret = devm_clk_hw_register(dev, &ahb->hw);
	if (ret)
		return ret;
	crg->clk_data->hws[SD5113_CLK_AHB] = &ahb->hw;

	/* spi_clk: single-bit gate, stock helper. */
	hw = devm_clk_hw_register_gate(dev, "spi_clk", "ahb_clk", 0,
					crg->base + SD5113_CRG_GATE_1C,
					SD5113_CRG_GATE_1C_SPI, 0, &crg->lock);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	crg->clk_data->hws[SD5113_CLK_SPI] = hw;

	/* i2c_clk: single-bit gate, stock helper. */
	hw = devm_clk_hw_register_gate(dev, "i2c_clk", "ahb_clk", 0,
					crg->base + SD5113_CRG_GATE_28,
					SD5113_CRG_GATE_28_I2C, 0, &crg->lock);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	crg->clk_data->hws[SD5113_CLK_I2C] = hw;

	timer_gate1_hw = devm_clk_hw_register_gate(dev, "timer_gate20",
						    "ahb_clk", 0,
						    crg->base + SD5113_CRG_GATE0,
						    SD5113_CRG_GATE0_TIMER_BIT1,
						    0, &crg->lock);
	if (IS_ERR(timer_gate1_hw))
		return PTR_ERR(timer_gate1_hw);

	hw = devm_clk_hw_register_gate(dev, "timer_clk", "timer_gate20", 0,
					crg->base + SD5113_CRG_GATE0,
					SD5113_CRG_GATE0_TIMER_BIT2, 0,
					&crg->lock);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	crg->clk_data->hws[SD5113_CLK_TIMER] = hw;

	ret = of_clk_add_hw_provider(dev->of_node, of_clk_hw_onecell_get,
				      crg->clk_data);
	if (ret)
		return ret;

	return 0;
}

static const struct of_device_id sd5113_crg_of_match[] = {
	{ .compatible = "hisilicon,sd5113-crg" },
	{ }
};
MODULE_DEVICE_TABLE(of, sd5113_crg_of_match);

static struct platform_driver sd5113_crg_driver = {
	.probe = sd5113_crg_probe,
	.driver = {
		.name = "sd5113-crg",
		.of_match_table = sd5113_crg_of_match,
	},
};
builtin_platform_driver(sd5113_crg_driver);
