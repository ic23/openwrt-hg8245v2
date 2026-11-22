/* SPDX-License-Identifier: GPL-2.0-only */
/*
  * ITU-T I.363.5 CRC-32, the one of crc32_i363_calc in drv_onu_gpe.c:
  * polynomial 0x04C11DB7, init and xorout 0xFFFFFFFF, not reflected.
  * Check of "123456789" = 0xFC891918. It is neither the Ethernet CRC nor the
  * AES-CMAC of omci-lib-go. The ioctl path seals with passthrough: the
  * comment on gpe_omci_msg says the CRC does not fit in length.
  */
#include "omci.h"

static uint32_t crc_table[256];
static int crc_ready;

static void crc_init(void)
{
	int i;

	if (crc_ready)
		return;
	for (i = 0; i < 256; i++) {
		uint32_t crc = (uint32_t)i << 24;
		int bit;

		for (bit = 0; bit < 8; bit++) {
			if (crc & 0x80000000u)
				crc = (crc << 1) ^ 0x04C11DB7u;
			else
				crc <<= 1;
		}
		crc_table[i] = crc;
	}
	crc_ready = 1;
}

uint32_t omci_aal5_crc32(const uint8_t *data, size_t len)
{
	uint32_t crc = 0xffffffffu;
	size_t i;

	crc_init();
	if (!data)
		len = 0;
	for (i = 0; i < len; i++) {
		unsigned idx = ((crc >> 24) ^ data[i]) & 0xffu;
		crc = (crc << 8) ^ crc_table[idx];
	}
	return crc ^ 0xffffffffu;
}

static void passthrough_seal(uint8_t frame[OMCI_FRAME_LEN])
{
	(void)frame;
}

static int passthrough_verify(const uint8_t *frame, size_t len)
{
	(void)frame;
	(void)len;
	return 1;
}

static void aal5_seal(uint8_t frame[OMCI_FRAME_LEN])
{
	omci_put_be32(frame + 44, omci_aal5_crc32(frame, 44));
}

static int aal5_verify(const uint8_t *frame, size_t len)
{
	uint32_t got;

	if (!frame || len < OMCI_FRAME_LEN)
		return 0;
	got = omci_be32(frame + 44);
	return omci_aal5_crc32(frame, 44) == got;
}

const struct omci_integrity omci_passthrough = {
	passthrough_seal,
	passthrough_verify,
};

const struct omci_integrity omci_aal5 = {
	aal5_seal,
	aal5_verify,
};
