/* SPDX-License-Identifier: GPL-2.0-only */
/*
  * Software download (G.988 A.2.27-A.2.31) in "dump" mode: if a directory is
  * configured, the image the OLT sends is accepted, saved to
  * <dir>/swdl-<epoch>-<inst>.bin with a .json next to it, and the End download CRC
  * is checked. Activate and Commit are always rejected: the image is never
  * applied and ME 7 keeps reporting the current version. Without a directory,
  * everything answers NOT_SUPPORTED as before.
  *
  * Baseline content: Start = [0] window-1, [1..4] size; Section = [0] section
  * number, [1..31] data (no AR except the last of the window); End =
  * [0..3] I.363.5 CRC-32 of the image, [4..7] size.
  */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "omci.h"

#define SWDL_SECTION 31u
#define SWDL_MAX (64u * 1024u * 1024u)

static void swdl_close(struct omci_swdl *s)
{
	if (s->fd >= 0)
		close(s->fd);
	s->fd = -1;
}

/* Also closes a download in progress; s must be zeroed or initialised. */
void omci_swdl_init(struct omci_swdl *s, const char *dir)
{
	if (s->dir[0])
		swdl_close(s);
	memset(s, 0, sizeof *s);
	s->fd = -1;
	if (dir && *dir)
		snprintf(s->dir, sizeof s->dir, "%s", dir);
}

static void start(struct omci_swdl *s, uint16_t inst, const uint8_t *c, uint8_t *out)
{
	uint32_t size = omci_be32(c + 1);

	swdl_close(s);
	if (size == 0 || size > SWDL_MAX) {
		out[0] = OMCI_RC_PARAMETER;
		return;
	}
	s->started = (uint32_t)time(NULL);
	s->inst = inst;
	s->size = size;
	s->window = (unsigned)c[0] + 1u;
	s->base = 0;
	s->sections = 0;
	snprintf(s->path, sizeof s->path, "%s/swdl-%u-%04x.bin", s->dir,
		 (unsigned)s->started, inst);
	s->fd = open(s->path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (s->fd < 0) {
		fprintf(stderr, "swdl: %s: %s\n", s->path, strerror(errno));
		out[0] = OMCI_RC_PROCESSING;
		return;
	}
	fprintf(stderr, "swdl: the OLT starts a software download: ME 7 inst 0x%04x, %u bytes, "
		"window %u -> %s (will not be applied)\n", inst, size, s->window, s->path);
	out[0] = OMCI_RC_SUCCESS;
	out[1] = c[0];
	out[2] = 1;
	out[3] = (uint8_t)(inst >> 8);
	out[4] = (uint8_t)inst;
	out[5] = OMCI_RC_SUCCESS;
}

/* Called for every section, with or without AR. Returns 1 if it wrote a reply. */
static int section(struct omci_swdl *s, const uint8_t *c, int ack, uint8_t *out)
{
	uint64_t off = (uint64_t)s->base + (uint64_t)c[0] * SWDL_SECTION;
	size_t n = SWDL_SECTION;
	int ok = s->fd >= 0 && c[0] < s->window && off < s->size;

	if (ok) {
		if (off + n > s->size)
			n = (size_t)(s->size - off);
		ok = pwrite(s->fd, c + 1, n, (off_t)off) == (ssize_t)n;
		s->sections++;
	}
	if (!ack)
		return 0;
	if (ok)
		s->base += ((uint32_t)c[0] + 1u) * SWDL_SECTION;
	out[0] = ok ? OMCI_RC_SUCCESS : OMCI_RC_PROCESSING;
	out[1] = c[0];
	return 1;
}

static void end(struct omci_swdl *s, uint16_t inst, const uint8_t *c, uint8_t *out)
{
	uint32_t want = omci_be32(c), size = omci_be32(c + 4), crc = 0;
	uint8_t *img = NULL;
	char meta[sizeof s->path + 8];
	FILE *f;
	int ok = s->fd >= 0 && size == s->size;

	if (ok && (img = malloc(size)) && pread(s->fd, img, size, 0) == (ssize_t)size)
		crc = omci_aal5_crc32(img, size);
	else
		ok = 0;
	free(img);
	if (s->fd >= 0) {
		snprintf(meta, sizeof meta, "%.*s.json", (int)(strlen(s->path) - 4), s->path);
		f = fopen(meta, "w");
		if (f) {
			fprintf(f, "{\"me7_instance\": %u, \"size\": %u, \"window\": %u, "
				"\"sections\": %u, \"crc_olt\": \"%08x\", \"crc_local\": \"%08x\", "
				"\"crc_ok\": %s, \"started\": %u, \"ended\": %u, \"applied\": false}\n",
				s->inst, s->size, s->window, s->sections, want, crc,
				ok && crc == want ? "true" : "false", s->started,
				(unsigned)time(NULL));
			fclose(f);
		}
		fprintf(stderr, "swdl: download finished %s: %u bytes, OLT CRC %08x local %08x (%s)\n",
			s->path, s->size, want, crc, ok && crc == want ? "OK" : "NO coincide");
	}
	swdl_close(s);
	out[0] = ok && crc == want ? OMCI_RC_SUCCESS : OMCI_RC_PROCESSING;
	out[1] = 1;
	out[2] = (uint8_t)(inst >> 8);
	out[3] = (uint8_t)inst;
	out[4] = out[0];
}

/* Returns 1 if a response was produced in out (OMCI_CONTENTS_LEN bytes, zeroed by caller). */
int omci_swdl_handle(struct omci_swdl *s, uint8_t action, uint16_t inst, int ack,
		     const uint8_t *c, uint8_t *out)
{
	if (!s->dir[0]) {	/* dump mode off: previous behaviour */
		out[0] = OMCI_RC_NOT_SUPPORTED;
		return ack;
	}
	switch (action) {
	case OMCI_MT_START_SW_DOWNLOAD:
		start(s, inst, c, out);
		return 1;
	case OMCI_MT_DOWNLOAD_SECTION:
		return section(s, c, ack, out);
	case OMCI_MT_END_SW_DOWNLOAD:
		end(s, inst, c, out);
		return 1;
	case OMCI_MT_ACTIVATE_SW:
	case OMCI_MT_COMMIT_SW:
		fprintf(stderr, "swdl: the OLT asks to %s the image ME 7 inst 0x%04x: rejected\n",
			action == OMCI_MT_ACTIVATE_SW ? "activar" : "confirmar", inst);
		out[0] = OMCI_RC_PROCESSING;
		return 1;
	}
	return 0;
}
