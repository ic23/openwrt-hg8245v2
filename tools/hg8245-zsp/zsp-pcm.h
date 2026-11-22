/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HG8245_ZSP_PCM_H
#define HG8245_ZSP_PCM_H

#include <linux/io.h>
#include <linux/types.h>

struct gpio_desc;
struct regmap;

struct hg8245_pcm {
	struct regmap *crg;
	void __iomem *hw;
	struct gpio_desc *pads[4];
	u32 old_funcs;
	u32 old_width;
	u32 old_mask;
	u32 old_clock;
	u32 old_enable;
	bool chain;
	bool armed;
};

int hg8245_pcm_prepare(struct hg8245_pcm *pcm, struct regmap *crg, void __iomem *hw,
		       bool chain);
int hg8245_pcm_finish(struct hg8245_pcm *pcm, bool restore);

#endif
