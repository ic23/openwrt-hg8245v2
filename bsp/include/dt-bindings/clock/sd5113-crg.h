/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _DT_BINDINGS_CLOCK_SD5113_CRG_H
#define _DT_BINDINGS_CLOCK_SD5113_CRG_H

#define SD5113_CLK_AHB		0	/* always-on, rate-only, no gate bit known */
#define SD5113_CLK_SPI		1	/* CRG+0x1C, BIT(26) - CONFIRMED round 14 */
#define SD5113_CLK_I2C		2	/* CRG+0x28, BIT(27) - CONFIRMED round 14 */
#define SD5113_CLK_TIMER	3	 

#define SD5113_CLK_NUM_CLOCKS	4


#endif /* _DT_BINDINGS_CLOCK_SD5113_CRG_H */
