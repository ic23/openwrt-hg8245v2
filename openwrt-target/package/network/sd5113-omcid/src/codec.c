/* SPDX-License-Identifier: GPL-2.0-only */
#include "omci.h"

#include <string.h>

int omci_decode(const uint8_t *raw, size_t len, struct omci_frame *out)
{
	if (!raw || !out || len < 40)
		return 0;
	if (raw[3] != OMCI_DEVICE_BASELINE)
		return 0;
	out->tci = omci_be16(raw);
	out->message_type = raw[2];
	out->device_id = raw[3];
	out->class_id = omci_be16(raw + 4);
	out->instance = omci_be16(raw + 6);
	memcpy(out->contents, raw + 8, OMCI_CONTENTS_LEN);
	return 1;
}

void omci_encode(const struct omci_frame *frame, const struct omci_integrity *ig,
		 uint8_t out[OMCI_FRAME_LEN])
{
	memset(out, 0, OMCI_FRAME_LEN);
	if (!frame)
		return;
	omci_put_be16(out, frame->tci);
	out[2] = frame->message_type;
	out[3] = frame->device_id;
	omci_put_be16(out + 4, frame->class_id);
	omci_put_be16(out + 6, frame->instance);
	memcpy(out + 8, frame->contents, OMCI_CONTENTS_LEN);
	omci_put_be32(out + 40, OMCI_BASELINE_LENGTH);
	if (ig && ig->seal)
		ig->seal(out);
}

uint8_t omci_response_type(uint8_t mt)
{
	return (uint8_t)((mt & OMCI_MT_MASK) | OMCI_AK);
}
