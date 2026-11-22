/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HG8245_ZSP_HARDWARE_H
#define HG8245_ZSP_HARDWARE_H
#include <linux/io.h>
#include <linux/firmware.h>
#include <linux/regmap.h>
#include <linux/sizes.h>
#include "zsp-loader.h"
#include "zsp-hif.h"
#define ZSP_FIRMWARE		"hg8245/zsp_min.out"
#define ZSP_STOCK_BYTES		322212U
#define ZSP_CRG_SELECT		0x18
#define ZSP_CRG_CONTROL		0x1c
#define ZSP_CRG_CLK_SET		0x24
#define ZSP_CRG_CLK_STATUS	0x2c
#define ZSP_CRG_SRAM		0x38
#define ZSP_CRG_SRAM_CFG		0x3c
#define ZSP_CRG_ROUTE		0x40
#define ZSP_CRG_IRQ_CLEAR	0x4c
#define ZSP_CRG_IRQ_CTRL	0x50
#define ZSP_CONTROL_MASK	GENMASK(31, 29)
#define ZSP_CONTROL_UBOOT	BIT(31)
#define ZSP_CONTROL_RUN		(BIT(31) | BIT(29))
#define ZSP_CONTROL_STOP	BIT(30)
#define ZSP_HW_ENABLE		BIT(11)
#define ZSP_CLOCK_BIT		BIT(31)
#define ZSP_SELECT_BIT		BIT(27)
#define ZSP_SRAM_BIT		BIT(27)
#define ZSP_SRAM_CFG_MASK	GENMASK(21, 0)
#define ZSP_SRAM_CFG_OLD		0x2897U
#define ZSP_MAILBOX_LEN		0x50U
#define ZSP_SHARED_BYTES	SZ_4M
#define ZSP_HW_BASE		0x43a00000U
#define ZSP_HW_BYTES		0x128U
#define ZSP_HW_INIT		0x64U
#define ZSP_HW_INIT_BITS		0x001f5f40U
#define ZSP_INTC_BASE		0x40000000U
#define ZSP_INTC_MASK		0x08U
#define ZSP_INTC_RAW_STATUS	0x18U
#define ZSP_INTC_IRQ_BIT		BIT(20)
#define ZSP_BOOT_TIMEOUT_MS	3000
#define ZSP_HIF_REPLY_TIMEOUT_MS	1000
#define ZSP_VERSION_SEQUENCE	1
#define ZSP_TDM_SEQUENCE	2

struct zsp_window {
	u32 base;
	u32 size;
	void __iomem *io;
};

struct zsp_hardware {
	struct zsp_window shared;
	struct zsp_window inst;
	struct zsp_window data;
	struct zsp_window hw;
	void __iomem *intc;
	struct regmap *crg;
	unsigned int verified_writes;
	bool run_unverified;
};

enum zsp_hif_wait {
	ZSP_WAIT_BOOT,
	ZSP_WAIT_VERSION,
	ZSP_WAIT_TDM,
};

int zsp_window_open(struct zsp_window *w);
void zsp_window_close(struct zsp_window *w);
int zsp_readback_write(void *ctx, enum zsp_target target, u32 addr,
			      const void *payload, size_t len);
int zsp_hold_for_staging(struct regmap *crg, bool check_only);
int zsp_map_irq_guard(struct zsp_hardware *hw);
int zsp_boot_preflight(struct zsp_hardware *hw);
int zsp_enable_boot_irqs(struct zsp_hardware *hw);
int zsp_enter_run(struct zsp_hardware *hw);
void zsp_zero_boot_scratch(struct zsp_window *shared);
int zsp_wait_hif(struct zsp_hardware *hw, enum zsp_hif_wait what,
			 unsigned int sequence);
int zsp_send_hif(struct zsp_hardware *hw, const u8 *frame, size_t len);
int zsp_apply_stock_tdm(struct zsp_hardware *hw);
int zsp_map_firmware(const struct firmware *fw, struct zsp_hardware *hw, bool with_highway);
void zsp_unmap_firmware(struct zsp_hardware *hw);
#endif
