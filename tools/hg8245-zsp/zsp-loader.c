// SPDX-License-Identifier: GPL-2.0-only
#include "zsp-loader.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif

#define ELF_HEADER_BYTES	52
#define ELF_SECTION_BYTES	40
#define ELF_MAX_SECTIONS	64
#define EM_ZSP			79
#define SHT_PROGBITS		1
#define SHT_STRTAB		3
#define ZSP_BANK_BYTES		0x40000U
#define ZSP_MAILBOX_BYTES	0x50U
#define ARRAY_LEN(a)		(sizeof(a) / sizeof((a)[0]))

struct named_section {
	const char *name;
	zsp_u32 bank;
};

static const struct named_section resident[] = {
	{ ".exttext_5", ZSP_INST_PHYS },
	{ ".text", ZSP_INST_PHYS },
	{ ".InstBlock0", ZSP_INST_PHYS },
	{ ".Interrupt", ZSP_INST_PHYS },
	{ ".InstBlock1", ZSP_INST_PHYS },
	{ ".data", ZSP_DATA_PHYS },
	{ ".DataBlock0", ZSP_DATA_PHYS },
	{ ".DataBlock1", ZSP_DATA_PHYS },
	{ ".T38Data", ZSP_DATA_PHYS },
	{ ".TdTable", ZSP_DATA_PHYS },
	{ ".DtmfTable", ZSP_DATA_PHYS },
	{ ".ANRTable", ZSP_DATA_PHYS },
};

_Static_assert(ARRAY_LEN(resident) == ZSP_RESIDENT_COUNT, "resident plan size");

static const char *const inst_overlays[12] = {
	".exttext_0", ".exttext_2", NULL, ".exttext_4",
	".exttext_1", ".exttext_3", ".exttext_6", ".exttext_7",
	".exttext_5", ".exttext_9", ".exttext_10", NULL,
};

static const char *const data_overlays[12] = {
	".extdata_0", ".extdata_2", NULL, ".extdata_4", ".extdata_1",
};

struct elf_sections {
	const zsp_u8 *file;
	size_t len;
	const zsp_u8 *table;
	zsp_u32 count;
	const char *str;
	zsp_u32 str_len;
};

static zsp_u32 get_le16(const zsp_u8 *p)
{
	return (zsp_u32)p[0] | ((zsp_u32)p[1] << 8);
}

static zsp_u32 get_le32(const zsp_u8 *p)
{
	return get_le16(p) | (get_le16(p + 2) << 16);
}

static void put_le32(zsp_u8 *p, zsp_u32 value)
{
	unsigned int i;

	for (i = 0; i != 4; ++i)
		p[i] = (zsp_u8)(value >> (8 * i));
}

static int within(size_t off, size_t size, size_t total)
{
	return off <= total && size <= total - off;
}

static int sections_init(const zsp_u8 *p, size_t len, struct elf_sections *es)
{
	static const zsp_u8 ident[] = { 0x7f, 'E', 'L', 'F', 1, 1, 1 };
	zsp_u32 offset, index;
	const zsp_u8 *str;

	if (!p || len < ELF_HEADER_BYTES || len > ZSP_IMAGE_MAX ||
	    memcmp(p, ident, sizeof(ident)) || get_le16(p + 16) != 2 ||
	    get_le16(p + 18) != EM_ZSP || get_le32(p + 20) != 1 ||
	    get_le16(p + 40) != ELF_HEADER_BYTES ||
	    get_le16(p + 46) != ELF_SECTION_BYTES)
		return -ENOEXEC;

	offset = get_le32(p + 32);
	es->count = get_le16(p + 48);
	index = get_le16(p + 50);
	if (!es->count || es->count > ELF_MAX_SECTIONS ||
	    !index || index >= es->count ||
	    !within(offset, es->count * ELF_SECTION_BYTES, len))
		return -ENOEXEC;
	es->file = p;
	es->len = len;
	es->table = p + offset;
	str = es->table + index * ELF_SECTION_BYTES;
	offset = get_le32(str + 16);
	es->str_len = get_le32(str + 20);
	if (get_le32(str + 4) != SHT_STRTAB || !es->str_len ||
	    !within(offset, es->str_len, len))
		return -ENOEXEC;
	es->str = (const char *)p + offset;
	return 0;
}

/* SD5112_DRV_CarryCode finds sections by name, not by PT_LOAD. */
static int section_find(const struct elf_sections *es, const char *name,
			zsp_u32 *offset, zsp_u32 *addr, zsp_u32 *size)
{
	const zsp_u8 *match = NULL;
	zsp_u32 i;

	for (i = 1; i < es->count; ++i) {
		const zsp_u8 *sh = es->table + i * ELF_SECTION_BYTES;
		zsp_u32 n = get_le32(sh);

		if (n >= es->str_len ||
		    !memchr(es->str + n, '\0', es->str_len - n))
			return -ENOEXEC;
		if (strcmp(es->str + n, name))
			continue;
		if (match || get_le32(sh + 4) != SHT_PROGBITS)
			return -ENOEXEC;
		match = sh;
	}
	if (!match)
		return -ENOEXEC;
	*addr = get_le32(match + 12);
	*offset = get_le32(match + 16);
	*size = get_le32(match + 20);
	if (!*size || !within(*offset, *size, es->len) ||
	    !within(*addr, *size, ZSP_BANK_BYTES))
		return -ENOEXEC;
	return 0;
}

static int overlap(zsp_u32 a, zsp_u32 a_size, zsp_u32 b, zsp_u32 b_size)
{
	return a < b + b_size && b < a + a_size;
}

static int make_overlay(const struct elf_sections *es, const char *const names[12],
			zsp_u32 bank, zsp_u8 out[ZSP_OVERLAY_BYTES])
{
	unsigned int i;

	for (i = 0; i != 12; ++i) {
		zsp_u32 offset, addr, size;
		zsp_u8 *rec = out + i * 12;
		int ret;

		if (!names[i])
			continue;
		ret = section_find(es, names[i], &offset, &addr, &size);
		if (ret)
			return ret;
		if (!within(addr, (size + 7) & ~7U, ZSP_BANK_BYTES) ||
		    !within(offset, (size + 7) & ~7U, es->len) ||
		    overlap(bank + addr, (size + 7) & ~7U,
			    ZSP_MAILBOX_PHYS, ZSP_MAILBOX_BYTES))
			return -ENOEXEC;
		put_le32(rec, ZSP_STAGE_PHYS + offset);
		put_le32(rec + 4, bank + addr);
		put_le32(rec + 8, (size + 7) / 8);
	}
	return 0;
}

int zsp_prepare(const void *image, size_t len, struct zsp_load_plan *plan)
{
	struct elf_sections es;
	unsigned int i, j;
	int ret;

	if (!plan)
		return -EINVAL;
	memset(plan, 0, sizeof(*plan));
	ret = sections_init(image, len, &es);
	if (ret)
		return ret;

	for (i = 0; i != ARRAY_LEN(resident); ++i) {
		struct zsp_resident *entry = &plan->resident[i];
		zsp_u32 addr, size;

		ret = section_find(&es, resident[i].name, &entry->file_offset,
				   &addr, &size);
		if (ret)
			return ret;
		/* The stock load helper copies the section rounded up to four. */
		entry->dest = resident[i].bank + addr;
		entry->bytes = (size + 3) & ~3U;
		if ((addr & 3) || !within(addr, entry->bytes, ZSP_BANK_BYTES) ||
		    !within(entry->file_offset, entry->bytes, es.len) ||
		    overlap(entry->dest, entry->bytes,
			    ZSP_MAILBOX_PHYS, ZSP_MAILBOX_BYTES))
			return -ENOEXEC;
		for (j = 0; j < i; ++j)
			if (overlap(entry->dest, entry->bytes,
				    plan->resident[j].dest, plan->resident[j].bytes))
				return -ENOEXEC;
	}

	plan->overlay_phys[0] = ZSP_STAGE_PHYS + (((zsp_u32)len + 3) & ~3U);
	plan->overlay_phys[1] = plan->overlay_phys[0] + ZSP_OVERLAY_BYTES;
	ret = make_overlay(&es, inst_overlays, ZSP_INST_PHYS, plan->overlays[0]);
	if (ret)
		return ret;
	return make_overlay(&es, data_overlays, ZSP_DATA_PHYS, plan->overlays[1]);
}

int zsp_stage(const void *image, size_t len, zsp_write_fn write, void *ctx)
{
	const zsp_u8 *file = image;
	struct zsp_load_plan plan;
	unsigned int i;
	int ret;
	zsp_u8 ptrs[8];

	if (!write)
		return -EINVAL;
	ret = zsp_prepare(image, len, &plan);
	if (ret)
		return ret;

	ret = write(ctx, ZSP_TARGET_STAGE, ZSP_STAGE_PHYS, file, len);
	if (ret)
		return ret;
	for (i = 0; i != ZSP_RESIDENT_COUNT; ++i) {
		const struct zsp_resident *entry = &plan.resident[i];

		ret = write(ctx, ZSP_TARGET_LOCAL, entry->dest,
			    file + entry->file_offset, entry->bytes);
		if (ret)
			return ret;
	}
	for (i = 0; i != 2; ++i) {
		ret = write(ctx, ZSP_TARGET_STAGE, plan.overlay_phys[i],
			    plan.overlays[i], ZSP_OVERLAY_BYTES);
		if (ret)
			return ret;
		put_le32(ptrs + i * 4, plan.overlay_phys[i]);
	}
	/* Publish only fully constructed tables. Reset is still asserted. */
	ret = write(ctx, ZSP_TARGET_MAILBOX, ZSP_MAILBOX_PHYS + 0x24,
		    ptrs, sizeof(ptrs));
	if (ret)
		return ret;
	put_le32(ptrs, ZSP_BOOTINFO_PHYS);
	return write(ctx, ZSP_TARGET_MAILBOX, ZSP_MAILBOX_PHYS + 0x4c, ptrs, 4);
}
