/* SPDX-License-Identifier: GPL-2.0-only */
/* Consumidor de /var/run/gpon/identity (contrato: gpon-identity/CONSUMERS.md). */
#ifndef OMCID_IDENTITY_H
#define OMCID_IDENTITY_H

#include <stddef.h>
#include <stdint.h>

#include "omci.h"

#define IDENT_OK 0
#define IDENT_ABSENT 1
#define IDENT_INVALID (-1)

#define IDK_SERIAL (1u << 0)
#define IDK_PLOAM (1u << 1)
#define IDK_VENDOR (1u << 2)
#define IDK_HW_VERSION (1u << 3)
#define IDK_EQUIPMENT (1u << 4)
#define IDK_OMCC (1u << 5)
#define IDK_SW0 (1u << 6)
#define IDK_SW1 (1u << 7)
#define IDK_SW_ACTIVE (1u << 8)
#define IDK_SW_COMMITTED (1u << 9)
#define IDK_SW_VALID0 (1u << 10)
#define IDK_SW_VALID1 (1u << 11)

struct identity {
	unsigned have;     /* IDK_* present and valid */
	unsigned rejected; /* IDK_* present but invalid or repeated */
	int loose_mode;    /* the file is readable by group/others */
	uint8_t serial[8]; /* layout PLOAM: 4 ASCII + 4 bytes */
	uint8_t password[10]; /* SECRETO */
	char vendor_id[5];
	char hw_version[15];
	char equipment_id[21];
	uint8_t omcc_version;
	char sw_version[2][15];
	int sw_active;
	int sw_committed;
	int sw_valid[2]; /* ME 7 is_valid; 0 or 1 if IDK_SW_VALID* is in have */
};

/* Loads the file. IDENT_ABSENT if it does not exist; IDENT_INVALID if it is not a
 * regular file, is too large, has NUL or lines without '='. With
 * IDENT_INVALID `out` is left empty. Invalid keys are dropped one by one
 * (kept in `rejected`) and never replaced by invented values. */
int identity_load(const char *path, struct identity *out);

/* Wipes the contents with a write the compiler cannot optimise away. */
void identity_wipe(struct identity *id);

/* 1 if `fresh` carries a serial or password different from what is already programmed in
 * `applied` (an absent key does not count). That requires redoing the GTC. */
int identity_pon_changed(const struct identity *applied, const struct identity *fresh);

/* Copia serial y password presentes de `src` a `dst`. */
void identity_merge(struct identity *dst, const struct identity *src);

/* Applies the present keys to the MIB (ME 256, 257, 7). Returns how many
 * attributes it wrote. Absent ones are not zero-filled. */
int identity_apply_mib(struct omci_mib *m, const struct identity *id);

/* Writes an attribute into the baseline and the live MIB (mib.c). */
int omci_mib_set_base_attr(struct omci_mib *m, uint16_t class_id, uint16_t me_id,
			   unsigned index, const uint8_t *bytes, size_t n);

#endif
