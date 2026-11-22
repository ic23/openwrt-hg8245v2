/* SPDX-License-Identifier: GPL-2.0-only */
#include "service.h"
#include <string.h>

static int value(const struct omci_mib *m, int cls, int id, int attr)
{
	size_t n;
	const uint8_t *p = omci_mib_scalar(m, cls, id, attr, &n);
	return !p ? -1 : n == 1 ? p[0] : n == 2 ? omci_be16(p) : -1;
}

/* Translate the common symmetric untagged VEIP <-> C-tag rule. */
static int service_vid(const struct omci_mib *m, int veip, int bp, int *vid, int *pcp)
{
	uint16_t cls, id;
	size_t i;
	for (i = 0; omci_mib_at(m, i, &cls, &id); i++) {
		uint8_t row[16];
		uint32_t ti, to;
		int n, type, ptr;
		if (cls != 171)
			continue;
		type = value(m, cls, id, 0);
		ptr = value(m, cls, id, 6);
		if (!((type == 10 && ptr == veip) || (type == 0 && ptr == bp)))
			return OMCI_RC_NOT_SUPPORTED;
		n = omci_mib_table(m, cls, id, 5, row, sizeof(row));
		if (!n)
			continue;
		if (n != sizeof(row) || value(m, cls, id, 4) != 0 || *vid ||
		    row[0] >> 4 != 15 || row[4] >> 4 != 15 || (row[7] & 15))
			return OMCI_RC_NOT_SUPPORTED;
		to = omci_be32(row + 8);
		ti = omci_be32(row + 12);
		if ((to >> 30) || ((to >> 16) & 15) != 15 || ((ti >> 16) & 15) > 7 ||
		    ((ti & 7) != 4 && (ti & 7) != 6) ||
		    ((ti & 7) == 6 && value(m, cls, id, 3) != 0x8100))
			return OMCI_RC_NOT_SUPPORTED;
		*pcp = (ti >> 16) & 7;
		*vid = (ti >> 3) & 8191;
		if (*vid < 1 || *vid > 4094)
			return OMCI_RC_PARAMETER;
	}
	return OMCI_RC_SUCCESS;
}

/* The bridge investigates the TCI after upstream tagging and before
 * downstream tag removal (G.988 9.3.11). Preserve both port policies.
 */
static int bridge_filter(const struct omci_mib *m, int port, uint8_t out[26])
{
	size_t len;
	const uint8_t *list = omci_mib_scalar(m, 84, port, 0, &len);
	int op, count;

	if (!list)
		return OMCI_RC_SUCCESS;
	op = value(m, 84, port, 1);
	count = value(m, 84, port, 2);
	if (count < 0 || count > 12 || len != 24)
		return OMCI_RC_PARAMETER;
	switch (op) {
	case 3: case 4: op += 12; break;
	case 7: case 8: op += 10; break;
	case 11: case 12: op += 8; break;
	case 0x15: op = 2; break;
	default:
		if (op >= 0x1c && op <= 0x21)
			op -= 13;
	}
	if (op > 2 && (op < 0x0f || op > 0x14))
		return OMCI_RC_NOT_SUPPORTED;
	out[0] = op;
	out[1] = count;
	memcpy(out + 2, list, count * 2);
	return OMCI_RC_SUCCESS;
}

static int service_paths(const struct omci_mib *m, int type, int tp,
			 struct sd5113_service *svc)
{
	int p, mode;
	size_t len;

	if (type == 3) {
		const uint8_t *dscp = omci_mib_scalar(m, 130, tp, 10, &len);

		if (!dscp)
			return OMCI_RC_SUCCESS;
		mode = value(m, 130, tp, 9);
		svc->unmarked = mode == 0 ? SD5113_FROM_DSCP : value(m, 130, tp, 11);
		if (mode > 1 || svc->unmarked < 0 ||
		    (mode && svc->unmarked >= SD5113_PBITS) || len != sizeof(svc->dscp))
			return OMCI_RC_PARAMETER;
		memcpy(svc->dscp, dscp, sizeof(svc->dscp));
	}
	for (p = 0; p < SD5113_PBITS; p++) {
		int iw = type == 3 ? value(m, 130, tp, p + 1) : tp;
		int option, ctp, gem, alloc;

		if (iw == 0xffff || !omci_mib_contains(m, 266, iw))
			continue;
		option = value(m, 266, iw, 1);
		if (option != (type == 3 ? 5 : 1))
			return OMCI_RC_NOT_SUPPORTED;
		ctp = value(m, 266, iw, 0);
		gem = value(m, 268, ctp, 0);
		if (gem < 0)
			continue;
		if (gem >= 4095 || value(m, 268, ctp, 2) != 3)
			return OMCI_RC_NOT_SUPPORTED;
		alloc = value(m, 262, value(m, 268, ctp, 1), 0);
		if (alloc < 0 || alloc == 255 || alloc == 65535)
			continue;
		if (alloc >= 4095)
			return OMCI_RC_PARAMETER;
		for (int q = 0; q < p; q++)
			if ((svc->paths[q] >> 12) == (unsigned)gem &&
			    (svc->paths[q] & 4095) != (unsigned)alloc)
				return OMCI_RC_PARAMETER;
		svc->paths[p] = (uint32_t)gem << 12 | alloc;
	}
	return OMCI_RC_SUCCESS;
}

int sd5113_service(const struct omci_mib *m, struct sd5113_service *svc)
{
	size_t i, j;
	uint16_t cls, uni, ani;
	int selected = 0;

	memset(svc, 0, sizeof(*svc));
	memset(svc->paths, 0xff, sizeof(svc->paths));
	if (value(m, 256, 0, 6) == 1)
		return OMCI_RC_SUCCESS;
	for (i = 0; omci_mib_at(m, i, &cls, &uni); i++) {
		int bridge, veip;
		if (cls != 47 || value(m, 47, uni, 2) != 11)
			continue;
		veip = value(m, 47, uni, 3);
		bridge = value(m, 47, uni, 0);
		if (value(m, 329, veip, 0) != 0 || !omci_mib_contains(m, 45, bridge))
			continue;
		for (j = 0; omci_mib_at(m, j, &cls, &ani); j++) {
			int rc, type, tp;
			if (cls != 47 || ani == uni || value(m, 47, ani, 0) != bridge)
				continue;
			type = value(m, 47, ani, 2);
			if (selected || (type != 3 && type != 5))
				return OMCI_RC_NOT_SUPPORTED;
			selected = 1;
			tp = value(m, 47, ani, 3);
			rc = service_vid(m, veip, uni, &svc->vid, &svc->tag_pcp);
			if (!rc)
				rc = bridge_filter(m, uni, svc->filters[0]);
			if (!rc)
				rc = bridge_filter(m, ani, svc->filters[1]);
			if (!rc)
				rc = service_paths(m, type, tp, svc);
			if (rc)
				return rc;
		}
	}
	return OMCI_RC_SUCCESS;
}
