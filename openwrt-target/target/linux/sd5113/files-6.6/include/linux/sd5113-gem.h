/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_SD5113_GEM_H
#define _LINUX_SD5113_GEM_H
#include <linux/types.h>

struct sd5113_gem_profile {
	u32 route[8];
	u8 dscp[64];
	u8 unmarked, tag_pcp;
	u8 filters[2][26];
};

int sd5113_eth_set_gem(const struct sd5113_gem_profile *profile);
#endif
