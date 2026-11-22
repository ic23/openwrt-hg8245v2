/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_GPIO_SD5113_H
#define _LINUX_GPIO_SD5113_H

#include <linux/types.h>

struct gpio_desc;

/* The caller must hold all descriptors; mask/value are bank-local bits. */
int sd5113_gpio_set_funcs(struct gpio_desc * const *pads, unsigned int count,
			 u32 mask, u32 value, u32 *old);
/* Read all eight FUNC bits in the bank containing a held descriptor. */
int sd5113_gpio_read_bank_funcs(struct gpio_desc *pad, u32 *funcs);

#endif
