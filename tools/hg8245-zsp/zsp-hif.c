// SPDX-License-Identifier: GPL-2.0-only
#include "zsp-hif.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

int zsp_hif_prepare_bootinfo(zsp_write_fn write, void *ctx)
{
	/*
	 * SD5112ADP_VOIP_AddrCfg provides the six PSTK addresses;
	 * SD_GetMemCfg supplies the last three words. All are physical
	 * addresses (or the high byte of one), not Linux virtual pointers.
	 */
	static const zsp_u32 words[] = {
		ZSP_BOOTINFO_PHYS + 0x24U,
		ZSP_BOOTINFO_PHYS + 0xf024U,
		ZSP_BOOTINFO_PHYS + 0xf124U,
		ZSP_BOOTINFO_PHYS + 0x141a4U,
		ZSP_BOOTINFO_PHYS + 0x5db24U,
		ZSP_BOOTINFO_PHYS + 0x65b24U,
		ZSP_HIF_DOWN_PHYS,
		ZSP_HIF_UP_PHYS,
		ZSP_HIF_DOWN_PHYS >> 24,
	};
	zsp_u8 block[ZSP_HIF_BOOTINFO_LEN];
	unsigned int i, b;

	_Static_assert(sizeof(words) == ZSP_HIF_BOOTINFO_LEN, "bootinfo layout");
	if (!write)
		return -EINVAL;
	for (i = 0; i < sizeof(words) / sizeof(words[0]); ++i)
		for (b = 0; b < 4; ++b)
			block[i * 4 + b] = (zsp_u8)(words[i] >> (b * 8));
	return write(ctx, ZSP_TARGET_BOOTINFO, ZSP_BOOTINFO_PHYS,
		     block, sizeof(block));
}

int zsp_hif_prepare_cursors(zsp_write_fn write, void *ctx)
{
	static const zsp_u8 empty[4];
	int ret;

	if (!write)
		return -EINVAL;
	ret = write(ctx, ZSP_TARGET_CURSOR, ZSP_HIF_DOWN_CURSOR,
		    empty, sizeof(empty));
	if (ret)
		return ret;
	return write(ctx, ZSP_TARGET_CURSOR, ZSP_HIF_UP_CURSOR, empty, sizeof(empty));
}

int zsp_hif_up_slot(unsigned int producer, unsigned int consumer, zsp_u32 *addr)
{
	if (!addr || producer >= ZSP_HIF_UP_SLOTS || consumer >= ZSP_HIF_UP_SLOTS)
		return -EINVAL;
	if (producer == consumer)
		return 0;
	*addr = ZSP_HIF_UP_PHYS + consumer * ZSP_HIF_SLOT_SIZE;
	return 1;
}

int zsp_hif_down_slot(unsigned int producer, unsigned int consumer,
		       zsp_u32 *addr, unsigned int *next)
{
	if (!addr || !next || producer >= ZSP_HIF_DOWN_SLOTS ||
	    consumer >= ZSP_HIF_DOWN_SLOTS)
		return -EINVAL;
	*next = (producer + 1) % ZSP_HIF_DOWN_SLOTS;
	if (*next == consumer)
		return -ENOSPC;
	*addr = ZSP_HIF_DOWN_PHYS + producer * ZSP_HIF_SLOT_SIZE;
	return 0;
}

int zsp_hif_make_version_request(unsigned int sequence,
				 zsp_u8 out[ZSP_HIF_VERSION_REQ_LEN])
{
	if (!out || !sequence || sequence >= 0xffff)
		return -EINVAL;
	/* HIF_SendControlMessage header and HIAPI_QueryDspVersionCmd TLV. */
	out[0] = 0xff;
	out[1] = 0x7f;
	out[2] = (zsp_u8)sequence;
	out[3] = (zsp_u8)(sequence >> 8);
	out[4] = 4;
	out[5] = 1;
	out[6] = 0x54;
	out[7] = 0;
	return 0;
}

static unsigned int hif_le16(const zsp_u8 *bytes)
{
	return bytes[0] | (bytes[1] << 8);
}

int zsp_hif_make_stock_tdm_request(unsigned int sequence,
				   zsp_u8 out[ZSP_HIF_TDM_REQ_LEN])
{
	unsigned int i;

	if (!out || !sequence || sequence >= 0xffff)
		return -EINVAL;
	/* Exact chip-misc payload observed on this board; replace sequence only. */
	out[0] = 0xff;
	out[1] = 0x7f;
	out[2] = (zsp_u8)sequence;
	out[3] = (zsp_u8)(sequence >> 8);
	out[4] = 12;
	out[5] = 1;
	out[6] = 0x1b;
	for (i = 7; i < ZSP_HIF_TDM_REQ_LEN; ++i)
		out[i] = 0;
	return 0;
}

int zsp_hif_tdm_reply(const void *slot, size_t len, unsigned int sequence,
		       unsigned int *status)
{
	const zsp_u8 *bytes = slot;
	size_t pos = 4;
	unsigned int record;
	int result = 0;

	if (!bytes || !status || len < 8 || len > ZSP_HIF_SLOT_SIZE)
		return -EINVAL;
	if (bytes[1] != 0x7f)
		return -EPROTO;
	if (bytes[0] != 0xff || hif_le16(bytes + 2) != sequence)
		return 0;
	for (record = 0; record < 20 && pos <= len - 4; ++record) {
		const zsp_u8 *tlv = bytes + pos;
		unsigned int size = tlv[0];

		if (size < 4 || size > len - pos)
			return -EPROTO;
		if (tlv[2] == 0x1b && (tlv[3] == 2 || tlv[3] == 0x12)) {
			if (size < 6)
				return -EPROTO;
			*status = hif_le16(tlv + 4);
			if (*status)
				return -EREMOTEIO;
			result = 1;
		}
		if (tlv[1])
			return result;
		pos += size;
	}
	return -EPROTO;
}

int zsp_hif_version_reply(const void *slot, size_t len, unsigned int sequence,
			   struct zsp_hif_version *version)
{
	const zsp_u8 *bytes = slot;
	size_t pos = 4;
	unsigned int record;
	int result = 0;

	if (!bytes || !version || len < 8 || len > ZSP_HIF_SLOT_SIZE)
		return -EINVAL;
	if (bytes[1] != 0x7f)
		return -EPROTO;
	if (bytes[0] != 0xff || hif_le16(bytes + 2) != sequence)
		return 0;
	for (record = 0; record < 20 && pos <= len - 4; ++record) {
		const zsp_u8 *tlv = bytes + pos;
		unsigned int size = tlv[0];

		if (size < 4 || size > len - pos)
			return -EPROTO;
		if (tlv[2] == 0x54 && (tlv[3] == 2 || tlv[3] == 0x12)) {
			if (size < 6)
				return -EPROTO;
			version->status = hif_le16(tlv + 4);
			if (version->status) {
				result = -EREMOTEIO;
			} else if (size >= 26) {
				/* Stock callback: 16-bit status, then version words. */
				version->year = hif_le16(tlv + 6);
				version->date = hif_le16(tlv + 8);
				version->main = hif_le16(tlv + 10);
				version->sub = hif_le16(tlv + 12);
				result = 1;
			}
		}
		if (tlv[1])
			return result;
		pos += size;
	}
	return -EPROTO;
}

int zsp_hif_is_boot_complete(const void *slot, size_t len)
{
	const zsp_u8 *bytes = slot;
	size_t pos = 4;
	unsigned int record;
	int boot = 0;

	if (!bytes || len < 8 || len > ZSP_HIF_SLOT_SIZE)
		return -EINVAL;
	if (bytes[1] != 0x7f)
		return -EPROTO;

	/* CS_GetMsgTotalLen: four-byte header, then length-prefixed TLVs. */
	for (record = 0; record < 20 && pos <= len - 4; ++record) {
		unsigned int length = bytes[pos];

		if (length < 4 || length > len - pos)
			return -EPROTO;
		/*
		 * HIAPI_ZspBootCompleteMapReg registers the unsolicited
		 * (class 1, tag 1, kind 1, channel 0xff) indication.
		 * HIF_BootCompleteHandler also checks host context state;
		 * that host field is not part of this wire record.
		 */
		if (bytes[0] == 0xff && bytes[pos + 2] == 1 && bytes[pos + 3] == 1)
			boot = 1;
		if (bytes[pos + 1])
			return boot;
		pos += length;
	}
	return -EPROTO;
}
