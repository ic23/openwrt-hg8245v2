// SPDX-License-Identifier: GPL-2.0
/* HiSilicon SD5113 (Huawei HG8245) Clock & Reset Generator (CRG) */

#include <linux/clk-provider.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
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
#define SD5113_CRG_I2C_BIT		27
#define SD5113_CRG_CLK_SET		0x24
#define SD5113_CRG_CLK_CLEAR		0x28
#define SD5113_CRG_CLK_STATUS		0x2c
#define SD5113_CRG_SPI_BIT		26

struct sd5113_gate {
	struct clk_hw hw;
	struct regmap *crg;
	unsigned int reg;
	unsigned int mask;
	bool sticky;
	bool command_gate;
};

#define to_sd5113_gate(_hw) container_of(_hw, struct sd5113_gate, hw)

#define SD5113_AHB_RATE_HZ	133250000UL

static int sd5113_gate_enable(struct clk_hw *hw)
{
	struct sd5113_gate *gate = to_sd5113_gate(hw);

	if (gate->command_gate)
		return regmap_write(gate->crg, SD5113_CRG_CLK_SET, gate->mask);

	return regmap_set_bits(gate->crg, gate->reg, gate->mask);
}

static void sd5113_gate_disable(struct clk_hw *hw)
{
	struct sd5113_gate *gate = to_sd5113_gate(hw);

	if (gate->sticky)
		return;

	if (gate->command_gate)
		regmap_write(gate->crg, SD5113_CRG_CLK_CLEAR, gate->mask);
	else
		regmap_clear_bits(gate->crg, gate->reg, gate->mask);
}

static int sd5113_gate_is_enabled(struct clk_hw *hw)
{
	struct sd5113_gate *gate = to_sd5113_gate(hw);
	unsigned int value;
	int ret;

	ret = regmap_read(gate->crg,
			  gate->command_gate ? SD5113_CRG_CLK_STATUS : gate->reg,
			  &value);
	return ret ? ret : !!(value & gate->mask);
}

static const struct clk_ops sd5113_gate_ops = {
	.enable = sd5113_gate_enable,
	.disable = sd5113_gate_disable,
	.is_enabled = sd5113_gate_is_enabled,
};

static struct clk_hw *sd5113_register_gate(struct device *dev, struct regmap *crg,
					  const char *name, const struct clk_hw *parent,
					  unsigned int reg, unsigned int bit,
					  bool sticky, bool command_gate)
{
	struct clk_init_data init = {
		.name = name,
		.ops = &sd5113_gate_ops,
		.parent_hws = &parent,
		.num_parents = 1,
		.flags = CLK_IGNORE_UNUSED,
	};
	struct sd5113_gate *gate;
	int ret;

	gate = devm_kzalloc(dev, sizeof(*gate), GFP_KERNEL);
	if (!gate)
		return ERR_PTR(-ENOMEM);

	gate->crg = crg;
	gate->reg = reg;
	gate->mask = BIT(bit);
	gate->sticky = sticky;
	gate->command_gate = command_gate;
	gate->hw.init = &init;
	ret = devm_clk_hw_register(dev, &gate->hw);
	return ret ? ERR_PTR(ret) : &gate->hw;
}

static int sd5113_crg_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct clk_hw_onecell_data *data;
	struct clk_hw *ahb, *hw, *timer_gate1;
	struct regmap *crg;

	crg = syscon_node_to_regmap(dev->of_node);
	if (IS_ERR(crg))
		return dev_err_probe(dev, PTR_ERR(crg), "cannot get CRG\n");

	data = devm_kzalloc(dev, struct_size(data, hws, SD5113_CLK_NUM_CLOCKS),
			    GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->num = SD5113_CLK_NUM_CLOCKS;

	ahb = devm_clk_hw_register_fixed_rate(dev, "sd5113-ahb", NULL, 0,
					     SD5113_AHB_RATE_HZ);
	if (IS_ERR(ahb))
		return PTR_ERR(ahb);
	data->hws[SD5113_CLK_AHB] = ahb;

	hw = sd5113_register_gate(dev, crg, "sd5113-spi", ahb,
				  0, SD5113_CRG_SPI_BIT, true, true);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	data->hws[SD5113_CLK_SPI] = hw;

	hw = sd5113_register_gate(dev, crg, "sd5113-i2c", ahb,
				  0, SD5113_CRG_I2C_BIT, false, true);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	data->hws[SD5113_CLK_I2C] = hw;

	timer_gate1 = sd5113_register_gate(dev, crg, "sd5113-timer-pre", ahb,
					   SD5113_CRG_GATE0, SD5113_CRG_GATE0_TIMER_BIT1,
					   false, false);
	if (IS_ERR(timer_gate1))
		return PTR_ERR(timer_gate1);

	hw = sd5113_register_gate(dev, crg, "sd5113-timer", timer_gate1,
				  SD5113_CRG_GATE0, SD5113_CRG_GATE0_TIMER_BIT2,
				  false, false);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	data->hws[SD5113_CLK_TIMER] = hw;

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, data);
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
