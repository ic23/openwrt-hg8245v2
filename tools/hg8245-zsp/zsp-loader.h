/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HG8245_ZSP_LOADER_H
#define HG8245_ZSP_LOADER_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef u8 zsp_u8;
typedef u32 zsp_u32;
#else
#include <stddef.h>
#include <stdint.h>
typedef uint8_t zsp_u8;
typedef uint32_t zsp_u32;
#endif

#define ZSP_SHARED_PHYS		0x93400000U
#define ZSP_STAGE_PHYS		(ZSP_SHARED_PHYS + 0x300000U)
#define ZSP_INST_PHYS		0x50000000U
#define ZSP_DATA_PHYS		0x50040000U
#define ZSP_MAILBOX_PHYS	0x50045000U
#define ZSP_BOOTINFO_PHYS	(ZSP_SHARED_PHYS + 0x18a000U)
#define ZSP_IMAGE_MAX		0x00080000U
#define ZSP_OVERLAY_BYTES	0x90U
#define ZSP_RESIDENT_COUNT	12

enum zsp_target {
	ZSP_TARGET_STAGE,
	ZSP_TARGET_LOCAL,
	ZSP_TARGET_MAILBOX,
	ZSP_TARGET_CURSOR,
	ZSP_TARGET_BOOTINFO,
};

struct zsp_resident {
	zsp_u32 file_offset;
	zsp_u32 dest;
	zsp_u32 bytes;
};

struct zsp_load_plan {
	zsp_u32 overlay_phys[2];
	struct zsp_resident resident[ZSP_RESIDENT_COUNT];
	zsp_u8 overlays[2][ZSP_OVERLAY_BYTES];
};

/*
 * Returns zero only for a complete validated layout. The plan is not
 * usable on error. All addresses are physical ARM-side destinations.
 */
int zsp_prepare(const void *image, size_t len, struct zsp_load_plan *plan);

typedef int (*zsp_write_fn)(void *ctx, enum zsp_target target, zsp_u32 addr,
			    const void *data, size_t len);
int zsp_stage(const void *image, size_t len, zsp_write_fn write, void *ctx);

#endif
