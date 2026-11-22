/* SPDX-License-Identifier: GPL-2.0-only */
#include "omci.h"

#define SD5113_PBITS 8
#define SD5113_FROM_DSCP 8
#define SD5113_NO_GEM UINT32_MAX

struct sd5113_service {
	uint32_t paths[SD5113_PBITS]; /* GEM[23:12], Alloc-ID[11:0] */
	int vid, tag_pcp, unmarked;
	uint8_t dscp[24];
	uint8_t filters[2][26]; /* operation, count, 12 network-order TCIs */
};
int sd5113_service(const struct omci_mib *m, struct sd5113_service *service);
