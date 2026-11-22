/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HG8245_ZSP_HIF_H
#define HG8245_ZSP_HIF_H

#include "zsp-loader.h"

#define ZSP_HIF_DOWN_PHYS	(ZSP_SHARED_PHYS + 0x100000U)
#define ZSP_HIF_UP_PHYS		(ZSP_SHARED_PHYS + 0x102000U)
#define ZSP_HIF_SLOT_SIZE	0x100U
#define ZSP_HIF_DOWN_SLOTS	8U
#define ZSP_HIF_UP_SLOTS	32U
#define ZSP_HIF_DOWN_CURSOR	(ZSP_HIF_DOWN_PHYS + 0x80cU)
#define ZSP_HIF_UP_CURSOR	(ZSP_HIF_UP_PHYS + 0x200cU)
#define ZSP_HIF_BOOTINFO_LEN	0x24U
#define ZSP_HIF_VERSION_REQ_LEN	8U
#define ZSP_HIF_TDM_REQ_LEN	16U

struct zsp_hif_version {
	unsigned int status;
	unsigned int year;
	unsigned int date;
	unsigned int main;
	unsigned int sub;
};

int zsp_hif_prepare_cursors(zsp_write_fn write, void *ctx);
int zsp_hif_prepare_bootinfo(zsp_write_fn write, void *ctx);

/* Return 0 for empty, 1 and slot address for data, negative for invalid. */
int zsp_hif_up_slot(unsigned int producer, unsigned int consumer, zsp_u32 *addr);
int zsp_hif_down_slot(unsigned int producer, unsigned int consumer,
		       zsp_u32 *addr, unsigned int *next);
int zsp_hif_make_version_request(unsigned int sequence,
				 zsp_u8 out[ZSP_HIF_VERSION_REQ_LEN]);
int zsp_hif_make_stock_tdm_request(unsigned int sequence,
				   zsp_u8 out[ZSP_HIF_TDM_REQ_LEN]);
/* 1 for a matching successful 0x1b reply, 0 for another frame. */
int zsp_hif_tdm_reply(const void *slot, size_t len, unsigned int sequence,
		       unsigned int *status);
/* 1 for a complete version reply, 0 for another frame, negative for error. */
int zsp_hif_version_reply(const void *slot, size_t len, unsigned int sequence,
			   struct zsp_hif_version *version);

/*
 * Decode a stable copy of one posted slot. Return 1 for boot complete,
 * 0 for another valid HIF message, negative for malformed input.
 * Does not advance cursors or acknowledge interrupts.
 */
int zsp_hif_is_boot_complete(const void *slot, size_t len);

#endif
