/* SPDX-License-Identifier: GPL-2.0-only */
#include "omci.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OMCI_MAX_INST 256
#define OMCI_MAX_ROWS 32
#define OMCI_MAX_UPLOAD 1024
#define OMCI_INI_MAX (256u * 1024u)
#define TOK_RAW 96
#define TOK_DEC 64
#define TOK_MAX 24

struct omci_table {
	uint8_t n;
	uint8_t row_size;
	uint8_t rows[OMCI_MAX_ROWS][OMCI_VALUE_MAX];
};

struct omci_inst {
	uint16_t class_id;
	uint16_t me_id;
	uint8_t baseline;
	uint8_t used;
	uint8_t scalar[OMCI_MAX_ATTR][OMCI_VALUE_MAX];
	struct omci_table *table[OMCI_MAX_ATTR];
};

struct omci_upload_rec {
	uint16_t class_id;
	uint16_t me_id;
	uint16_t mask;
	uint8_t len;
	uint8_t bytes[OMCI_UPLOAD_NEXT_BUDGET];
};

struct omci_mib {
	struct omci_inst live[OMCI_MAX_INST];
	uint16_t order[OMCI_MAX_INST];
	uint16_t n;
	struct omci_inst base[OMCI_MAX_INST];
	uint16_t n_base;
	struct omci_upload_rec upload[OMCI_MAX_UPLOAD];
	uint16_t n_upload;
	uint8_t clock[7];
	uint8_t has_clock;
	uint8_t voice_noted;
	struct omci_hook hook;
	struct omci_swdl swdl;
};

_Static_assert(sizeof(struct omci_mib) < (1024u * 1024u), "the MIB fits in 1 MiB");

struct tok {
	char raw[TOK_RAW];
	uint8_t dec[TOK_DEC];
	unsigned nraw;
	unsigned ndec;
	int escaped;
	int quoted;
};

static void set_err(char *err, size_t errlen, const char *msg)
{
	if (!err || errlen == 0)
		return;
	snprintf(err, errlen, "%s", msg ? msg : "error");
}

static void set_err_line(char *err, size_t errlen, unsigned line, const char *msg)
{
	if (!err || errlen == 0)
		return;
	snprintf(err, errlen, "line %u: %s", line, msg ? msg : "error");
}

static void free_inst_tables(struct omci_inst *in)
{
	unsigned i;

	if (!in)
		return;
	for (i = 0; i < OMCI_MAX_ATTR; i++) {
		free(in->table[i]);
		in->table[i] = NULL;
	}
}

static void free_inst_range(struct omci_inst *arr, unsigned n)
{
	unsigned i;

	for (i = 0; i < n; i++)
		free_inst_tables(&arr[i]);
}

static void mib_wipe(struct omci_mib *m)
{
	if (!m)
		return;
	free_inst_range(m->live, OMCI_MAX_INST);
	free_inst_range(m->base, OMCI_MAX_INST);
	{
		struct omci_hook keep = m->hook;

		memset(m, 0, sizeof *m);
		m->hook = keep;
	}
}

struct omci_mib *omci_mib_new(void)
{
	struct omci_mib *m = calloc(1, sizeof(struct omci_mib));

	if (m)
		omci_swdl_init(&m->swdl, NULL);
	return m;
}

void omci_mib_free(struct omci_mib *m)
{
	if (!m)
		return;
	mib_wipe(m);
	omci_swdl_init(&m->swdl, NULL);
	free(m);
}

static struct omci_inst *find_mut(struct omci_mib *m, uint16_t class_id, uint16_t me_id)
{
	uint16_t i;

	if (!m)
		return NULL;
	for (i = 0; i < m->n; i++) {
		struct omci_inst *in = &m->live[m->order[i]];

		if (in->used && in->class_id == class_id && in->me_id == me_id)
			return in;
	}
	return NULL;
}

static const struct omci_inst *find_const(const struct omci_mib *m, uint16_t class_id,
					 uint16_t me_id)
{
	return find_mut((struct omci_mib *)m, class_id, me_id);
}

static int set_scalar(struct omci_inst *in, const struct omci_attr *attr, unsigned idx,
		      const uint8_t *bytes, size_t n)
{
	if (!in || !attr || !bytes || idx >= OMCI_MAX_ATTR)
		return 0;
	if (attr->kind == OMCI_KIND_TABLE)
		return 0;
	if (n != attr->size || n > OMCI_VALUE_MAX)
		return 0;
	memcpy(in->scalar[idx], bytes, n);
	return 1;
}

static int push_row(struct omci_inst *in, const struct omci_attr *attr, unsigned idx,
		    const uint8_t *row, size_t n, int key_len)
{
	struct omci_table *t;
	unsigned i;

	if (!in || !attr || !row || idx >= OMCI_MAX_ATTR)
		return 0;
	if (attr->kind != OMCI_KIND_TABLE)
		return 0;
	if (n != attr->size || n > OMCI_VALUE_MAX)
		return 0;
	if (key_len > (int)n)
		return 0;
	t = in->table[idx];
	if (key_len == 8 && n == 16 && omci_be32(row + 8) == UINT32_MAX &&
	    omci_be32(row + 12) == UINT32_MAX) {
		for (i = 0; t && i < t->n; i++) {
			if (memcmp(t->rows[i], row, 8))
				continue;
			memmove(t->rows + i, t->rows + i + 1,
				(t->n - i - 1) * sizeof(t->rows[0]));
			t->n--;
			break;
		}
		return 1;
	}
	if (!t) {
		t = calloc(1, sizeof *t);
		if (!t)
			return 0;
		t->row_size = attr->size;
		in->table[idx] = t;
	}
	if (t->row_size != n)
		return 0;
	if (key_len >= 0) {
		for (i = 0; i < t->n; i++) {
			if (memcmp(t->rows[i], row, (size_t)key_len) == 0) {
				memcpy(t->rows[i], row, n);
				return 1;
			}
		}
	}
	if (t->n >= OMCI_MAX_ROWS || (key_len == 8 && t->n >= 1))
		return 0;
	memcpy(t->rows[t->n], row, n);
	t->n++;
	return 1;
}

static int dup_tables(struct omci_inst *dst, const struct omci_inst *src)
{
	unsigned i;

	memset(dst->table, 0, sizeof dst->table);
	for (i = 0; i < OMCI_MAX_ATTR; i++) {
		if (!src->table[i])
			continue;
		dst->table[i] = malloc(sizeof *dst->table[i]);
		if (!dst->table[i])
			return -1;
		memcpy(dst->table[i], src->table[i], sizeof *dst->table[i]);
	}
	return 0;
}

static void mib_reset(struct omci_mib *m)
{
	uint16_t i;

	free_inst_range(m->live, OMCI_MAX_INST);
	memset(m->live, 0, sizeof m->live);
	m->n = 0;
	m->n_upload = 0;
	for (i = 0; i < m->n_base; i++) {
		m->live[i] = m->base[i];
		if (dup_tables(&m->live[i], &m->base[i]) != 0) {
			free_inst_tables(&m->live[i]);
			memset(&m->live[i], 0, sizeof m->live[i]);
			m->live[i] = m->base[i];
			{
				unsigned a;
				for (a = 0; a < OMCI_MAX_ATTR; a++)
					m->live[i].table[a] = NULL;
			}
		}
		m->order[i] = i;
		m->n++;
	}
	m->voice_noted = 1;
}

static void bump_sync(struct omci_mib *m)
{
	struct omci_inst *onu;
	const struct omci_class *cl;
	uint8_t cur = 0;
	uint8_t next;
	uint8_t byte;

	onu = find_mut(m, 2, 0);
	cl = omci_class_by_id(2);
	if (!onu || !cl || cl->n_attrs < 1)
		return;
	cur = onu->scalar[0][0];
	next = (uint8_t)(cur + 1u);
	if (next == 0)
		next = 1;
	byte = next;
	set_scalar(onu, &cl->attrs[0], 0, &byte, 1);
}

static void set_sync(struct omci_mib *m, uint8_t value)
{
	struct omci_inst *onu;
	const struct omci_class *cl;

	onu = find_mut(m, 2, 0);
	cl = omci_class_by_id(2);
	if (!onu || !cl || cl->n_attrs < 1)
		return;
	set_scalar(onu, &cl->attrs[0], 0, &value, 1);
}

static int snapshot_base(struct omci_mib *m)
{
	uint16_t i;

	free_inst_range(m->base, OMCI_MAX_INST);
	memset(m->base, 0, sizeof m->base);
	m->n_base = m->n;
	for (i = 0; i < m->n; i++) {
		const struct omci_inst *src = &m->live[m->order[i]];
		struct omci_inst *dst = &m->base[i];

		*dst = *src;
		if (dup_tables(dst, src) != 0) {
			free_inst_tables(dst);
			return -1;
		}
	}
	return 0;
}

size_t omci_mib_len(const struct omci_mib *m)
{
	return m ? m->n : 0;
}

int omci_mib_contains(const struct omci_mib *m, uint16_t class_id, uint16_t me_id)
{
	return find_const(m, class_id, me_id) != NULL;
}

int omci_mib_at(const struct omci_mib *m, size_t index, uint16_t *class_id, uint16_t *me_id)
{
	const struct omci_inst *in;

	if (!m || index >= m->n)
		return 0;
	in = &m->live[m->order[index]];
	if (class_id)
		*class_id = in->class_id;
	if (me_id)
		*me_id = in->me_id;
	return 1;
}

const uint8_t *omci_mib_scalar(const struct omci_mib *m, uint16_t class_id, uint16_t me_id,
			       unsigned index, size_t *len)
{
	const struct omci_inst *in;
	const struct omci_class *cl;

	in = find_const(m, class_id, me_id);
	cl = omci_class_by_id(class_id);
	if (!in || !cl || index >= cl->n_attrs || cl->attrs[index].kind == OMCI_KIND_TABLE)
		return NULL;
	if (len)
		*len = cl->attrs[index].size;
	return in->scalar[index];
}

int omci_mib_table(const struct omci_mib *m, uint16_t class_id, uint16_t me_id,
		   unsigned index, uint8_t *dst, size_t cap)
{
	const struct omci_inst *in;
	const struct omci_class *cl;
	const struct omci_table *t;
	size_t total = 0;
	size_t off = 0;
	unsigned i;

	in = find_const(m, class_id, me_id);
	cl = omci_class_by_id(class_id);
	if (!in || !cl || index >= cl->n_attrs || cl->attrs[index].kind != OMCI_KIND_TABLE)
		return -1;
	t = in->table[index];
	if (!t)
		return 0;
	total = (size_t)t->n * t->row_size;
	if (dst && cap > 0) {
		for (i = 0; i < t->n && off < cap; i++) {
			size_t n = t->row_size;

			if (n > cap - off)
				n = cap - off;
			memcpy(dst + off, t->rows[i], n);
			off += n;
		}
	}
	if (total > 0x7fffffffu)
		return -1;
	return (int)total;
}

int omci_mib_sync(const struct omci_mib *m, uint8_t *out)
{
	size_t len = 0;
	const uint8_t *s = omci_mib_scalar(m, 2, 0, 0, &len);

	if (!s || len < 1 || !out)
		return 0;
	*out = s[0];
	return 1;
}

int omci_mib_clock(const struct omci_mib *m, uint8_t out[7])
{
	if (!m || !m->has_clock || !out)
		return 0;
	memcpy(out, m->clock, 7);
	return 1;
}

int omci_mib_voice_noted(const struct omci_mib *m)
{
	return m && m->voice_noted;
}

void omci_mib_set_hook(struct omci_mib *m, const struct omci_hook *hook)
{
	if (!m)
		return;
	if (hook)
		m->hook = *hook;
	else
		memset(&m->hook, 0, sizeof m->hook);
}

static int run_hook(struct omci_mib *m, int ev, uint16_t class_id, uint16_t me_id)
{
	if (!m->hook.event)
		return OMCI_RC_SUCCESS;
	return m->hook.event(m->hook.ctx, m, ev, class_id, me_id);
}

static int hex_nibble(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int take_escape(const char *s, size_t n, size_t slash, uint8_t *byte, size_t *next,
		       char *err, size_t errlen)
{
	char e;

	if (slash + 1 >= n) {
		set_err(err, errlen, "escape truncado");
		return -1;
	}
	e = s[slash + 1];
	if (e == 's') {
		*byte = 0x20;
		*next = slash + 2;
		return 0;
	}
	if (e == '0') {
		*byte = 0;
		*next = slash + 2;
		return 0;
	}
	if (e == '\\' || e == '"' || e == '\'') {
		*byte = (uint8_t)e;
		*next = slash + 2;
		return 0;
	}
	if (e == 'x') {
		int hi, lo;

		if (slash + 3 >= n) {
			set_err(err, errlen, "\\x truncado");
			return -1;
		}
		hi = hex_nibble((unsigned char)s[slash + 2]);
		lo = hex_nibble((unsigned char)s[slash + 3]);
		if (hi < 0 || lo < 0) {
			set_err(err, errlen, "invalid \\x escape");
			return -1;
		}
		*byte = (uint8_t)((hi << 4) | lo);
		*next = slash + 4;
		return 0;
	}
	set_err(err, errlen, "escape desconocido");
	return -1;
}

static int unescape(const char *raw, size_t nraw, uint8_t *dec, unsigned *ndec,
		    char *err, size_t errlen)
{
	size_t i = 0;

	*ndec = 0;
	while (i < nraw) {
		if (raw[i] != '\\') {
			if (*ndec >= TOK_DEC) {
				set_err(err, errlen, "token demasiado largo");
				return -1;
			}
			dec[(*ndec)++] = (uint8_t)raw[i];
			i++;
			continue;
		}
		{
			uint8_t byte = 0;
			size_t next = 0;

			if (take_escape(raw, nraw, i, &byte, &next, err, errlen) != 0)
				return -1;
			if (*ndec >= TOK_DEC) {
				set_err(err, errlen, "token demasiado largo");
				return -1;
			}
			dec[(*ndec)++] = byte;
			i = next;
		}
	}
	return 0;
}

static int scan_tokens(const char *line, struct tok *toks, unsigned *nout,
		       char *err, size_t errlen)
{
	size_t i = 0;
	size_t n = strlen(line);

	*nout = 0;
	while (i < n) {
		unsigned char c = (unsigned char)line[i];
		struct tok *tk;

		if (c == '#')
			break;
		if (c == ' ' || c == '\t' || c == '\r') {
			i++;
			continue;
		}
		if (*nout >= TOK_MAX) {
			set_err(err, errlen, "demasiados tokens");
			return -1;
		}
		tk = &toks[*nout];
		memset(tk, 0, sizeof *tk);
		if (c == '"' || c == '\'') {
			char quote = (char)c;
			size_t start;
			int closed = 0;

			i++;
			start = i;
			while (i < n) {
				if (line[i] == '\\') {
					uint8_t byte = 0;
					size_t next = 0;

					tk->escaped = 1;
					if (take_escape(line, n, i, &byte, &next, err, errlen) != 0)
						return -1;
					if (tk->ndec >= TOK_DEC) {
						set_err(err, errlen, "token demasiado largo");
						return -1;
					}
					tk->dec[tk->ndec++] = byte;
					i = next;
					continue;
				}
				if (line[i] == quote) {
					closed = 1;
					i++;
					break;
				}
				if (tk->ndec >= TOK_DEC) {
					set_err(err, errlen, "token demasiado largo");
					return -1;
				}
				tk->dec[tk->ndec++] = (uint8_t)line[i];
				i++;
			}
			if (!closed) {
				set_err(err, errlen, "unterminated quote");
				return -1;
			}
			if (i - 1 < start || (i - 1 - start) >= TOK_RAW) {
				set_err(err, errlen, "token demasiado largo");
				return -1;
			}
			tk->nraw = (unsigned)(i - 1 - start);
			memcpy(tk->raw, line + start, tk->nraw);
			tk->quoted = 1;
			(*nout)++;
			continue;
		}
		{
			size_t start = i;

			while (i < n) {
				unsigned char d = (unsigned char)line[i];

				if (d == ' ' || d == '\t' || d == '\r' || d == '#')
					break;
				if (d == '\\')
					tk->escaped = 1;
				i++;
			}
			if (i == start || (i - start) >= TOK_RAW) {
				set_err(err, errlen, "token demasiado largo");
				return -1;
			}
			tk->nraw = (unsigned)(i - start);
			memcpy(tk->raw, line + start, tk->nraw);
			if (unescape(tk->raw, tk->nraw, tk->dec, &tk->ndec, err, errlen) != 0)
				return -1;
			(*nout)++;
		}
	}
	return 0;
}

static int parse_mag(const char *body, size_t n, uint64_t *out, char *err, size_t errlen)
{
	uint64_t val = 0;
	size_t i;
	unsigned base = 10;

	if (n >= 2 && body[0] == '0' && (body[1] == 'x' || body[1] == 'X')) {
		if (n == 2) {
			set_err(err, errlen, "empty hexadecimal");
			return -1;
		}
		base = 16;
		for (i = 2; i < n; i++) {
			int d = hex_nibble((unsigned char)body[i]);

			if (d < 0) {
				set_err(err, errlen, "invalid hexadecimal");
				return -1;
			}
			if (val > (UINT64_MAX - (unsigned)d) / 16u) {
				set_err(err, errlen, "entero demasiado grande");
				return -1;
			}
			val = val * 16u + (unsigned)d;
		}
		*out = val;
		return 0;
	}
	if (n > 1 && body[0] == '0') {
		for (i = 0; i < n; i++) {
			unsigned char c = (unsigned char)body[i];

			if (c < '0' || c > '7') {
				set_err(err, errlen, "invalid octal");
				return -1;
			}
			if (val > (UINT64_MAX - (unsigned)(c - '0')) / 8u) {
				set_err(err, errlen, "entero demasiado grande");
				return -1;
			}
			val = val * 8u + (unsigned)(c - '0');
		}
		*out = val;
		return 0;
	}
	if (n == 0) {
		set_err(err, errlen, "empty number");
		return -1;
	}
	for (i = 0; i < n; i++) {
		unsigned char c = (unsigned char)body[i];

		if (c < '0' || c > '9') {
			set_err(err, errlen, "not an integer");
			return -1;
		}
		if (val > (UINT64_MAX - (unsigned)(c - '0')) / base) {
			set_err(err, errlen, "entero demasiado grande");
			return -1;
		}
		val = val * base + (unsigned)(c - '0');
	}
	*out = val;
	return 0;
}

static int encode_int(const char *raw, size_t nraw, size_t size, uint8_t *out,
		      char *err, size_t errlen)
{
	int neg = 0;
	const char *body = raw;
	size_t nbody = nraw;
	uint64_t val = 0;
	unsigned bits;
	uint64_t mask;
	unsigned i;

	if (size == 0 || size > 8) {
		set_err(err, errlen, "unsupported integer size");
		return -1;
	}
	if (nraw > 0 && raw[0] == '-') {
		neg = 1;
		body = raw + 1;
		nbody = nraw - 1;
		if (nbody == 0) {
			set_err(err, errlen, "empty number");
			return -1;
		}
	}
	if (parse_mag(body, nbody, &val, err, errlen) != 0)
		return -1;
	bits = (unsigned)size * 8u;
	mask = (bits == 64u) ? UINT64_MAX : ((1ull << bits) - 1ull);
	if (neg) {
		uint64_t min_mag = 1ull << (bits - 1u);

		if (val > min_mag) {
			set_err(err, errlen, "does not fit as signed");
			return -1;
		}
		val = (uint64_t)(0ull - val) & mask;
	} else if (val > mask) {
		set_err(err, errlen, "does not fit");
		return -1;
	}
	for (i = 0; i < size; i++) {
		unsigned shift = (unsigned)(size - 1u - i) * 8u;
		out[i] = (uint8_t)((val >> shift) & 0xffu);
	}
	return 0;
}

static int hex_exact(const char *raw, size_t nraw, size_t size, uint8_t *out)
{
	const char *hex = raw;
	size_t nhex = nraw;
	size_t i;

	if (nraw >= 2 && raw[0] == '0' && (raw[1] == 'x' || raw[1] == 'X')) {
		hex = raw + 2;
		nhex = nraw - 2;
	} else {
		return 0;
	}
	if (size == 0 || nhex != size * 2u)
		return 0;
	for (i = 0; i < size; i++) {
		int hi = hex_nibble((unsigned char)hex[i * 2u]);
		int lo = hex_nibble((unsigned char)hex[i * 2u + 1u]);

		if (hi < 0 || lo < 0)
			return 0;
		out[i] = (uint8_t)((hi << 4) | lo);
	}
	return 1;
}

static int assign_int(const struct tok *tk, size_t size, uint8_t *out, char *err, size_t errlen)
{
	if (tk->quoted || tk->escaped) {
		if (tk->ndec > size) {
			set_err(err, errlen, "blob does not fit in the integer");
			return -1;
		}
		memset(out, 0, size);
		if (tk->ndec)
			memcpy(out + (size - tk->ndec), tk->dec, tk->ndec);
		return 0;
	}
	return encode_int(tk->raw, tk->nraw, size, out, err, errlen);
}

static int assign_octets(const struct tok *tk, size_t size, uint8_t *out, char *err, size_t errlen)
{
	if (!tk->quoted && !tk->escaped && hex_exact(tk->raw, tk->nraw, size, out))
		return 0;
	if (tk->ndec > size) {
		set_err(err, errlen, "string does not fit in the field");
		return -1;
	}
	memset(out, 0, size);
	if (tk->ndec)
		memcpy(out, tk->dec, tk->ndec);
	return 0;
}

static int parse_u16_tok(const struct tok *tk, uint16_t *out, char *err, size_t errlen)
{
	uint8_t bytes[2];

	if (tk->quoted || tk->escaped) {
		set_err(err, errlen, "non-numeric class/instance");
		return -1;
	}
	if (encode_int(tk->raw, tk->nraw, 2, bytes, err, errlen) != 0)
		return -1;
	*out = omci_be16(bytes);
	return 0;
}

int omci_mib_parse(struct omci_mib *m, const char *text, char *err, size_t errlen)
{
	const char *p;
	unsigned lineno = 0;
	char line_err[128];

	if (!m) {
		set_err(err, errlen, "mib nulo");
		return -1;
	}
	mib_wipe(m);
	if (!text)
		text = "";
	p = text;
	while (*p) {
		const char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);
		char line[1024];
		struct tok toks[TOK_MAX];
		unsigned nt = 0;
		unsigned ti;
		unsigned ai;
		uint16_t class_id = 0;
		uint16_t me_id = 0;
		const struct omci_class *cl;
		uint16_t slot;

		lineno++;
		if (len >= sizeof line) {
			set_err_line(err, errlen, lineno, "line too long");
			mib_wipe(m);
			return -1;
		}
		memcpy(line, p, len);
		line[len] = 0;
		if (len && line[len - 1] == '\r')
			line[len - 1] = 0;
		line_err[0] = 0;
		if (scan_tokens(line, toks, &nt, line_err, sizeof line_err) != 0) {
			set_err_line(err, errlen, lineno, line_err);
			mib_wipe(m);
			return -1;
		}
		if (nt == 0) {
			p = nl ? nl + 1 : p + len;
			if (!nl)
				break;
			continue;
		}
		if (nt < 2) {
			set_err_line(err, errlen, lineno, "faltan clase e instancia");
			mib_wipe(m);
			return -1;
		}
		if (parse_u16_tok(&toks[0], &class_id, line_err, sizeof line_err) != 0 ||
		    parse_u16_tok(&toks[1], &me_id, line_err, sizeof line_err) != 0) {
			set_err_line(err, errlen, lineno, line_err[0] ? line_err : "clase/instancia");
			mib_wipe(m);
			return -1;
		}
		cl = omci_class_by_id(class_id);
		if (!cl) {
			snprintf(line_err, sizeof line_err,
				 "class %u is not in the phase 1a table", class_id);
			set_err_line(err, errlen, lineno, line_err);
			mib_wipe(m);
			return -1;
		}
		if (find_mut(m, class_id, me_id)) {
			snprintf(line_err, sizeof line_err, "clase %u instancia %u duplicada",
				 class_id, me_id);
			set_err_line(err, errlen, lineno, line_err);
			mib_wipe(m);
			return -1;
		}
		if (m->n >= OMCI_MAX_INST) {
			set_err_line(err, errlen, lineno, "MIB lleno");
			mib_wipe(m);
			return -1;
		}
		slot = m->n;
		memset(&m->live[slot], 0, sizeof m->live[slot]);
		m->live[slot].class_id = class_id;
		m->live[slot].me_id = me_id;
		m->live[slot].baseline = 1;
		m->live[slot].used = 1;
		ti = 2;
		for (ai = 0; ai < cl->n_attrs; ai++) {
			uint8_t value[OMCI_VALUE_MAX];
			int rc;

			if (cl->attrs[ai].kind == OMCI_KIND_TABLE)
				continue;
			if (ti >= nt)
				break;
			if (cl->attrs[ai].kind == OMCI_KIND_INT)
				rc = assign_int(&toks[ti], cl->attrs[ai].size, value, line_err,
						sizeof line_err);
			else
				rc = assign_octets(&toks[ti], cl->attrs[ai].size, value, line_err,
						   sizeof line_err);
			if (rc != 0 || !set_scalar(&m->live[slot], &cl->attrs[ai], ai, value,
						   cl->attrs[ai].size)) {
				set_err_line(err, errlen, lineno,
					     line_err[0] ? line_err : "could not store the attribute");
				mib_wipe(m);
				return -1;
			}
			ti++;
		}
		if (ti < nt) {
			snprintf(line_err, sizeof line_err, "sobran %u tokens", nt - ti);
			set_err_line(err, errlen, lineno, line_err);
			mib_wipe(m);
			return -1;
		}
		m->order[m->n++] = slot;
		if (!nl)
			break;
		p = nl + 1;
	}
	if (snapshot_base(m) != 0) {
		set_err(err, errlen, "out of memory for the baseline");
		mib_wipe(m);
		return -1;
	}
	return 0;
}

int omci_mib_load_path(struct omci_mib *m, const char *path, char *err, size_t errlen)
{
	FILE *fp;
	char *buf;
	long sz;
	size_t nread;

	if (!m || !path) {
		set_err(err, errlen, "empty path");
		return -1;
	}
	fp = fopen(path, "rb");
	if (!fp) {
		set_err(err, errlen, "could not read the ini");
		return -1;
	}
	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		set_err(err, errlen, "could not read the ini");
		return -1;
	}
	sz = ftell(fp);
	if (sz < 0 || (unsigned long)sz > OMCI_INI_MAX) {
		fclose(fp);
		set_err(err, errlen, "ini demasiado grande");
		return -1;
	}
	if (fseek(fp, 0, SEEK_SET) != 0) {
		fclose(fp);
		set_err(err, errlen, "could not read the ini");
		return -1;
	}
	buf = malloc((size_t)sz + 1u);
	if (!buf) {
		fclose(fp);
		set_err(err, errlen, "out of memory");
		return -1;
	}
	nread = fread(buf, 1, (size_t)sz, fp);
	fclose(fp);
	if (nread != (size_t)sz) {
		free(buf);
		set_err(err, errlen, "incomplete ini read");
		return -1;
	}
	buf[nread] = 0;
	if (omci_mib_parse(m, buf, err, errlen) != 0) {
		free(buf);
		return -1;
	}
	free(buf);
	return 0;
}

static void result_only(uint8_t contents[OMCI_CONTENTS_LEN], uint8_t code)
{
	memset(contents, 0, OMCI_CONTENTS_LEN);
	contents[0] = code;
}

static void sink_rec(struct omci_sink *sink, const char *line)
{
	if (sink && sink->record)
		sink->record(sink->ctx, line);
}

static uint16_t known_mask(unsigned n_attrs)
{
	uint16_t mask = 0;
	unsigned i;

	if (n_attrs > 16)
		n_attrs = 16;
	for (i = 0; i < n_attrs; i++)
		mask = (uint16_t)(mask | omci_mask_bit(i));
	return mask;
}

static size_t table_octets(const struct omci_inst *in, unsigned index)
{
	const struct omci_table *t;

	if (!in || index >= OMCI_MAX_ATTR || !in->table[index])
		return 0;
	t = in->table[index];
	return (size_t)t->n * t->row_size;
}

static void copy_table(const struct omci_inst *in, unsigned index, uint8_t *dst, size_t cap,
		       size_t *out_len)
{
	const struct omci_table *t;
	size_t off = 0;
	unsigned i;

	*out_len = 0;
	if (!in || index >= OMCI_MAX_ATTR || !in->table[index])
		return;
	t = in->table[index];
	for (i = 0; i < t->n; i++) {
		if (off >= cap)
			break;
		if (off + t->row_size > cap)
			break;
		memcpy(dst + off, t->rows[i], t->row_size);
		off += t->row_size;
	}
	*out_len = off;
}

static void push_upload(struct omci_mib *m, uint16_t class_id, uint16_t me_id, uint16_t mask,
			const uint8_t *bytes, uint8_t blen)
{
	struct omci_upload_rec *rec;

	if (m->n_upload >= OMCI_MAX_UPLOAD)
		return;
	rec = &m->upload[m->n_upload++];
	rec->class_id = class_id;
	rec->me_id = me_id;
	rec->mask = mask;
	rec->len = blen;
	if (blen)
		memcpy(rec->bytes, bytes, blen);
}

static void capture_upload(struct omci_mib *m)
{
	uint16_t i;

	m->n_upload = 0;
	for (i = 0; i < m->n; i++) {
		struct omci_inst *in = &m->live[m->order[i]];
		const struct omci_class *cl = omci_class_by_id(in->class_id);
		uint16_t mask = 0;
		uint8_t bytes[OMCI_UPLOAD_NEXT_BUDGET];
		uint8_t blen = 0;
		int any = 0;
		unsigned ai;

		if (!cl)
			continue;
		for (ai = 0; ai < cl->n_attrs; ai++) {
			const struct omci_attr *attr = &cl->attrs[ai];
			const uint8_t *value;
			uint8_t vlen;

			if (!omci_attr_uploadable(attr))
				continue;
			value = in->scalar[ai];
			vlen = attr->size;
			if (blen != 0 && (unsigned)blen + vlen > OMCI_UPLOAD_NEXT_BUDGET) {
				push_upload(m, in->class_id, in->me_id, mask, bytes, blen);
				mask = 0;
				blen = 0;
				any = 1;
			}
			if ((unsigned)blen + vlen > OMCI_UPLOAD_NEXT_BUDGET)
				continue;
			mask = (uint16_t)(mask | omci_mask_bit(ai));
			memcpy(bytes + blen, value, vlen);
			blen = (uint8_t)(blen + vlen);
			any = 1;
		}
		if (blen == 0 && !any)
			push_upload(m, in->class_id, in->me_id, 0, NULL, 0);
		else if (blen != 0)
			push_upload(m, in->class_id, in->me_id, mask, bytes, blen);
	}
}

static void do_mib_reset(struct omci_mib *m, const struct omci_frame *req, struct omci_sink *sink,
			 uint8_t contents[OMCI_CONTENTS_LEN])
{
	if (!omci_class_by_id(req->class_id)) {
		result_only(contents, OMCI_RC_UNKNOWN_ME);
		return;
	}
	if (req->class_id != 2 || req->instance != 0) {
		result_only(contents, OMCI_RC_PROCESSING);
		return;
	}
	mib_reset(m);
	(void)run_hook(m, OMCI_HOOK_RESET, 2, 0);
	sink_rec(sink, OMCI_RESET_LINE);
	sink_rec(sink, OMCI_VOICE_PORT_LINE);
	result_only(contents, OMCI_RC_SUCCESS);
}

static void do_mib_upload(struct omci_mib *m, const struct omci_frame *req,
			  uint8_t contents[OMCI_CONTENTS_LEN])
{
	unsigned count;

	if (!omci_class_by_id(req->class_id)) {
		result_only(contents, OMCI_RC_UNKNOWN_ME);
		return;
	}
	if (req->class_id != 2 || req->instance != 0) {
		result_only(contents, OMCI_RC_PROCESSING);
		return;
	}
	capture_upload(m);
	count = m->n_upload;
	memset(contents, 0, OMCI_CONTENTS_LEN);
	omci_put_be16(contents, (uint16_t)(count > 65535u ? 65535u : count));
}

static void do_mib_upload_next(struct omci_mib *m, const struct omci_frame *req,
			       uint8_t contents[OMCI_CONTENTS_LEN])
{
	uint16_t seq;
	const struct omci_upload_rec *rec;
	uint8_t n;

	if (!omci_class_by_id(req->class_id)) {
		result_only(contents, OMCI_RC_UNKNOWN_ME);
		return;
	}
	if (req->class_id != 2 || req->instance != 0) {
		result_only(contents, OMCI_RC_PROCESSING);
		return;
	}
	seq = omci_be16(req->contents);
	memset(contents, 0, OMCI_CONTENTS_LEN);
	if (seq >= m->n_upload)
		return;
	rec = &m->upload[seq];
	omci_put_be16(contents, rec->class_id);
	omci_put_be16(contents + 2, rec->me_id);
	omci_put_be16(contents + 4, rec->mask);
	n = rec->len;
	if (n > OMCI_UPLOAD_NEXT_BUDGET)
		n = OMCI_UPLOAD_NEXT_BUDGET;
	if (n)
		memcpy(contents + 6, rec->bytes, n);
}

static void do_get_all_alarms(struct omci_mib *m, const struct omci_frame *req,
			      uint8_t contents[OMCI_CONTENTS_LEN])
{
	if (!omci_class_by_id(req->class_id)) {
		result_only(contents, OMCI_RC_UNKNOWN_ME);
		return;
	}
	if (!find_mut(m, req->class_id, req->instance)) {
		result_only(contents, OMCI_RC_UNKNOWN_INSTANCE);
		return;
	}
	memset(contents, 0, OMCI_CONTENTS_LEN);
}

static void do_sync_time(struct omci_mib *m, const struct omci_frame *req, struct omci_sink *sink,
			 uint8_t contents[OMCI_CONTENTS_LEN])
{
	if (!omci_class_by_id(req->class_id)) {
		result_only(contents, OMCI_RC_UNKNOWN_ME);
		return;
	}
	if (req->class_id != 256) {
		result_only(contents, OMCI_RC_NOT_SUPPORTED);
		return;
	}
	if (req->instance != 0 || !find_mut(m, 256, 0)) {
		result_only(contents, OMCI_RC_UNKNOWN_INSTANCE);
		return;
	}
	memcpy(m->clock, req->contents, 7);
	m->has_clock = 1;
	sink_rec(sink, OMCI_SYNC_LINE);
	result_only(contents, OMCI_RC_SUCCESS);
}

static void do_get(struct omci_mib *m, const struct omci_frame *req,
		   uint8_t contents[OMCI_CONTENTS_LEN])
{
	const struct omci_class *cl = omci_class_by_id(req->class_id);
	struct omci_inst *in;
	uint16_t asked;
	uint16_t unsupported;
	uint16_t out_mask = 0;
	uint8_t values[OMCI_GET_VALUE_BUDGET];
	unsigned vlen = 0;
	unsigned i;

	if (!cl) {
		result_only(contents, OMCI_RC_UNKNOWN_ME);
		return;
	}
	in = find_mut(m, req->class_id, req->instance);
	if (!in) {
		result_only(contents, OMCI_RC_UNKNOWN_INSTANCE);
		return;
	}
	asked = omci_be16(req->contents);
	unsupported = (uint16_t)(asked & (uint16_t)~known_mask(cl->n_attrs));
	for (i = 0; i < cl->n_attrs; i++) {
		const struct omci_attr *attr = &cl->attrs[i];
		uint16_t bit = omci_mask_bit(i);
		uint8_t chunk[4];
		const uint8_t *src;
		unsigned clen;

		if ((asked & bit) == 0)
			continue;
		if (!omci_attr_readable(attr)) {
			unsupported = (uint16_t)(unsupported | bit);
			continue;
		}
		if (attr->kind == OMCI_KIND_TABLE) {
			omci_put_be32(chunk, (uint32_t)table_octets(in, i));
			src = chunk;
			clen = 4;
		} else {
			src = in->scalar[i];
			clen = attr->size;
		}
		if (vlen + clen > OMCI_GET_VALUE_BUDGET)
			break;
		out_mask = (uint16_t)(out_mask | bit);
		memcpy(values + vlen, src, clen);
		vlen += clen;
	}
	memset(contents, 0, OMCI_CONTENTS_LEN);
	contents[0] = unsupported == 0 ? OMCI_RC_SUCCESS : OMCI_RC_ATTRIBUTE_FAILURE;
	omci_put_be16(contents + 1, out_mask);
	if (vlen)
		memcpy(contents + 3, values, vlen);
	if (unsupported != 0)
		omci_put_be16(contents + 28, unsupported);
}

static void do_get_next(struct omci_mib *m, const struct omci_frame *req,
			uint8_t contents[OMCI_CONTENTS_LEN])
{
	const struct omci_class *cl = omci_class_by_id(req->class_id);
	struct omci_inst *in;
	uint16_t asked;
	uint16_t seq;
	unsigned index;
	unsigned i;
	int found = 0;
	const struct omci_attr *attr;
	uint8_t data[OMCI_MAX_ROWS * OMCI_VALUE_MAX];
	size_t data_len = 0;
	uint32_t offset;
	unsigned n;

	if (!cl) {
		result_only(contents, OMCI_RC_UNKNOWN_ME);
		return;
	}
	in = find_mut(m, req->class_id, req->instance);
	if (!in) {
		result_only(contents, OMCI_RC_UNKNOWN_INSTANCE);
		return;
	}
	asked = omci_be16(req->contents);
	seq = omci_be16(req->contents + 2);
	if (__builtin_popcount((unsigned)asked) != 1) {
		result_only(contents, OMCI_RC_PARAMETER);
		return;
	}
	for (i = 0; i < cl->n_attrs; i++) {
		if (omci_mask_bit(i) == asked) {
			index = i;
			found = 1;
			break;
		}
	}
	if (!found) {
		result_only(contents, OMCI_RC_PARAMETER);
		return;
	}
	attr = &cl->attrs[index];
	if (!omci_attr_readable(attr)) {
		result_only(contents, OMCI_RC_PARAMETER);
		return;
	}
	if (attr->kind == OMCI_KIND_TABLE) {
		copy_table(in, index, data, sizeof data, &data_len);
	} else {
		/* size fits: the attribute maximum is OMCI_VALUE_MAX and data is larger. */
		memcpy(data, in->scalar[index], attr->size);
		data_len = attr->size;
	}
	offset = (uint32_t)seq * (uint32_t)OMCI_GET_NEXT_BUDGET;
	if ((size_t)offset >= data_len && !(data_len == 0 && offset == 0)) {
		result_only(contents, OMCI_RC_PARAMETER);
		return;
	}
	if (offset >= data_len) {
		result_only(contents, OMCI_RC_PARAMETER);
		return;
	}
	memset(contents, 0, OMCI_CONTENTS_LEN);
	contents[0] = OMCI_RC_SUCCESS;
	omci_put_be16(contents + 1, asked);
	n = (unsigned)(data_len - (size_t)offset);
	if (n > OMCI_GET_NEXT_BUDGET)
		n = OMCI_GET_NEXT_BUDGET;
	memcpy(contents + 3, data + offset, n);
}

struct parsed_set {
	unsigned n;
	unsigned index[OMCI_MAX_ATTR];
	uint8_t len[OMCI_MAX_ATTR];
	uint8_t bytes[OMCI_MAX_ATTR][OMCI_VALUE_MAX];
	uint16_t unsupported;
};

static int parse_set(const struct omci_frame *req, struct parsed_set *out, uint8_t *code)
{
	const struct omci_class *cl = omci_class_by_id(req->class_id);
	uint16_t asked;
	unsigned offset = 2;
	unsigned i;
	int any_w = 0;

	memset(out, 0, sizeof *out);
	if (!cl) {
		*code = OMCI_RC_UNKNOWN_ME;
		return -1;
	}
	for (i = 0; i < cl->n_attrs; i++) {
		if (omci_attr_writable(&cl->attrs[i]))
			any_w = 1;
	}
	if (!any_w) {
		*code = OMCI_RC_NOT_SUPPORTED;
		return -1;
	}
	asked = omci_be16(req->contents);
	out->unsupported = (uint16_t)(asked & (uint16_t)~known_mask(cl->n_attrs));
	for (i = 0; i < cl->n_attrs; i++) {
		const struct omci_attr *attr = &cl->attrs[i];
		uint16_t bit = omci_mask_bit(i);
		unsigned need = attr->size;

		if ((asked & bit) == 0)
			continue;
		if (offset + need > OMCI_CONTENTS_LEN || need > OMCI_VALUE_MAX) {
			*code = OMCI_RC_PARAMETER;
			return -1;
		}
		if (!omci_attr_writable(attr)) {
			out->unsupported = (uint16_t)(out->unsupported | bit);
			offset += need;
			continue;
		}
		if (out->n >= OMCI_MAX_ATTR) {
			*code = OMCI_RC_PARAMETER;
			return -1;
		}
		out->index[out->n] = i;
		out->len[out->n] = (uint8_t)need;
		memcpy(out->bytes[out->n], req->contents + offset, need);
		out->n++;
		offset += need;
	}
	return 0;
}

static void attribute_result(uint8_t contents[OMCI_CONTENTS_LEN], uint16_t unsupported,
			     uint16_t failed)
{
	result_only(contents, OMCI_RC_ATTRIBUTE_FAILURE);
	omci_put_be16(contents + 1, unsupported);
	omci_put_be16(contents + 3, failed);
}

static void do_set(struct omci_mib *m, const struct omci_frame *req, struct omci_sink *sink,
		   uint8_t contents[OMCI_CONTENTS_LEN])
{
	const struct omci_class *cl = omci_class_by_id(req->class_id);
	struct omci_inst *in;
	struct parsed_set parsed;
	uint8_t code = OMCI_RC_PARAMETER;
	int wrote_sync = 0;
	uint8_t sync_value = 0;
	int touched_admin = 0;
	int touched_alloc = 0;
	int touched_tcont_policy = 0;
	int touched_sched_policy = 0;
	struct omci_inst bak;
	int have_bak = 0;
	unsigned i;

	if (!cl) {
		result_only(contents, OMCI_RC_UNKNOWN_ME);
		return;
	}
	in = find_mut(m, req->class_id, req->instance);
	if (!in) {
		result_only(contents, OMCI_RC_UNKNOWN_INSTANCE);
		return;
	}
	if (parse_set(req, &parsed, &code) != 0) {
		result_only(contents, code);
		return;
	}
	if (m->hook.event && parsed.n > 0) { /* copy to undo if the GPE rejects */
		bak = *in;
		if (dup_tables(&bak, in) != 0) {
			free_inst_tables(&bak);
			result_only(contents, OMCI_RC_PROCESSING);
			return;
		}
		have_bak = 1;
	}
	for (i = 0; i < parsed.n; i++) {
		unsigned index = parsed.index[i];
		int stored;

		if (req->class_id == 2 && index == 0) {
			wrote_sync = 1;
			sync_value = parsed.bytes[i][0];
		}
		if (req->class_id == 256 && index == 6)
			touched_admin = 1;
		if (req->class_id == 262 && index == 0)
			touched_alloc = 1;
		/* QoS flexibility 24 says this policy is RW. It is stored, but
		 		 * gpe.c does not apply it: 262 only uses the alloc-id and 278 does not reach the hook. */
		if (req->class_id == 262 && index == 2)
			touched_tcont_policy = 1;
		if (req->class_id == 278 && index == 2)
			touched_sched_policy = 1;
		if (cl->attrs[index].kind == OMCI_KIND_TABLE) {
			int key_len = (req->class_id == 171 && index == 5) ? 8 : -1;
			stored = push_row(in, &cl->attrs[index], index, parsed.bytes[i],
					  parsed.len[i], key_len);
		} else {
			stored = set_scalar(in, &cl->attrs[index], index, parsed.bytes[i],
					    parsed.len[i]);
		}
		if (!stored) {
			if (have_bak) {
				free_inst_tables(in);
				*in = bak;
			}
			attribute_result(contents,
					 (uint16_t)(parsed.unsupported | omci_mask_bit(index)),
					 omci_mask_bit(index));
			return;
		}
	}
	if (have_bak) {
		int hrc = run_hook(m, OMCI_HOOK_SET, req->class_id, req->instance);

		if (hrc != OMCI_RC_SUCCESS) {
			free_inst_tables(in);
			*in = bak;
			(void)run_hook(m, OMCI_HOOK_SET, req->class_id, req->instance);
			result_only(contents, (uint8_t)hrc);
			return;
		}
		free_inst_tables(&bak);
	}
	if (wrote_sync)
		set_sync(m, sync_value);
	else if (parsed.n > 0)
		bump_sync(m);
	if (parsed.n > 0)
		omci_record_mutation(cl->apply, 0, touched_admin, touched_alloc, sink);
	if (touched_tcont_policy)
		sink_rec(sink, "T-CONT policy stored; the GPE does not apply it");
	if (touched_sched_policy)
		sink_rec(sink, "scheduler policy stored; the GPE does not apply it");
	if (parsed.unsupported == 0)
		result_only(contents, OMCI_RC_SUCCESS);
	else
		attribute_result(contents, parsed.unsupported, 0);
}

static int insert_created(struct omci_mib *m, const struct omci_class *cl, uint16_t me_id,
			  const struct parsed_set *fields)
{
	uint16_t slot;
	unsigned i;

	if (m->n >= OMCI_MAX_INST)
		return 0;
	for (slot = 0; slot < OMCI_MAX_INST; slot++) {
		if (!m->live[slot].used)
			break;
	}
	if (slot >= OMCI_MAX_INST)
		return 0;
	memset(&m->live[slot], 0, sizeof m->live[slot]);
	m->live[slot].class_id = cl->id;
	m->live[slot].me_id = me_id;
	m->live[slot].baseline = 0;
	m->live[slot].used = 1;
	if (cl->id == 171) {
		/* The VEIP currently implements one symmetric C-tag rule. */
		omci_put_be16(m->live[slot].scalar[1], 1);
		omci_put_be16(m->live[slot].scalar[2], 0x8100);
		omci_put_be16(m->live[slot].scalar[3], 0x8100);
	}
	for (i = 0; i < fields->n; i++) {
		if (!set_scalar(&m->live[slot], &cl->attrs[fields->index[i]], fields->index[i],
				fields->bytes[i], fields->len[i])) {
			memset(&m->live[slot], 0, sizeof m->live[slot]);
			return 0;
		}
	}
	m->order[m->n++] = slot;
	return 1;
}

static int delete_inst(struct omci_mib *m, uint16_t class_id, uint16_t me_id);

static void do_create(struct omci_mib *m, const struct omci_frame *req, struct omci_sink *sink,
		      uint8_t contents[OMCI_CONTENTS_LEN])
{
	const struct omci_class *cl = omci_class_by_id(req->class_id);
	struct parsed_set fields;
	unsigned offset = 0;
	unsigned i;

	if (!cl) {
		result_only(contents, OMCI_RC_UNKNOWN_ME);
		return;
	}
	if (!cl->allow_create) {
		result_only(contents, OMCI_RC_NOT_SUPPORTED);
		return;
	}
	if (find_mut(m, req->class_id, req->instance)) {
		result_only(contents, OMCI_RC_INSTANCE_EXISTS);
		return;
	}
	memset(&fields, 0, sizeof fields);
	for (i = 0; i < cl->n_attrs; i++) {
		const struct omci_attr *attr = &cl->attrs[i];
		unsigned need;
		unsigned k;

		if (!omci_attr_set_by_create(attr))
			continue;
		need = attr->size;
		if (offset > OMCI_CONTENTS_LEN || OMCI_CONTENTS_LEN - offset < need) {
			for (k = offset; k < OMCI_CONTENTS_LEN; k++) {
				if (req->contents[k] != 0) {
					result_only(contents, OMCI_RC_PARAMETER);
					return;
				}
			}
			break;
		}
		if (need > OMCI_VALUE_MAX || fields.n >= OMCI_MAX_ATTR) {
			result_only(contents, OMCI_RC_PARAMETER);
			return;
		}
		fields.index[fields.n] = i;
		fields.len[fields.n] = (uint8_t)need;
		memcpy(fields.bytes[fields.n], req->contents + offset, need);
		fields.n++;
		offset += need;
	}
	if (!insert_created(m, cl, req->instance, &fields)) {
		result_only(contents, OMCI_RC_PROCESSING);
		return;
	}
	{
		int hrc = run_hook(m, OMCI_HOOK_CREATE, req->class_id, req->instance);

		if (hrc != OMCI_RC_SUCCESS) {
			(void)delete_inst(m, req->class_id, req->instance);
			(void)run_hook(m, OMCI_HOOK_CREATE, req->class_id, req->instance);
			result_only(contents, (uint8_t)hrc);
			return;
		}
	}
	bump_sync(m);
	omci_record_mutation(cl->apply, 0, 0, 0, sink);
	result_only(contents, OMCI_RC_SUCCESS);
}

static int delete_inst(struct omci_mib *m, uint16_t class_id, uint16_t me_id)
{
	uint16_t i;

	for (i = 0; i < m->n; i++) {
		uint16_t slot = m->order[i];
		struct omci_inst *in = &m->live[slot];

		if (!in->used || in->class_id != class_id || in->me_id != me_id)
			continue;
		free_inst_tables(in);
		memset(in, 0, sizeof *in);
		memmove(&m->order[i], &m->order[i + 1], (size_t)(m->n - i - 1) * sizeof m->order[0]);
		m->n--;
		return 1;
	}
	return 0;
}

static void do_delete(struct omci_mib *m, const struct omci_frame *req, struct omci_sink *sink,
		      uint8_t contents[OMCI_CONTENTS_LEN])
{
	const struct omci_class *cl = omci_class_by_id(req->class_id);
	struct omci_inst *in;

	if (!cl) {
		result_only(contents, OMCI_RC_UNKNOWN_ME);
		return;
	}
	if (!cl->allow_delete) {
		result_only(contents, OMCI_RC_NOT_SUPPORTED);
		return;
	}
	in = find_mut(m, req->class_id, req->instance);
	if (!in) {
		result_only(contents, OMCI_RC_UNKNOWN_INSTANCE);
		return;
	}
	if (in->baseline) {
		result_only(contents, OMCI_RC_PROCESSING);
		return;
	}
	if (m->hook.event) {
		struct omci_inst bak = *in;
		int hrc;

		if (dup_tables(&bak, in) != 0) {
			free_inst_tables(&bak);
			result_only(contents, OMCI_RC_PROCESSING);
			return;
		}
		if (!delete_inst(m, req->class_id, req->instance)) {
			free_inst_tables(&bak);
			result_only(contents, OMCI_RC_PROCESSING);
			return;
		}
		hrc = run_hook(m, OMCI_HOOK_DELETE, req->class_id, req->instance);
		if (hrc != OMCI_RC_SUCCESS) { /* restores the instance (at the end of the order) */
			uint16_t slot;

			for (slot = 0; slot < OMCI_MAX_INST && m->live[slot].used; slot++)
				;
			if (slot < OMCI_MAX_INST && m->n < OMCI_MAX_INST) {
				m->live[slot] = bak;
				m->order[m->n++] = slot;
			} else {
				free_inst_tables(&bak);
			}
			(void)run_hook(m, OMCI_HOOK_DELETE, req->class_id, req->instance);
			result_only(contents, (uint8_t)hrc);
			return;
		}
		free_inst_tables(&bak);
	} else if (!delete_inst(m, req->class_id, req->instance)) {
		result_only(contents, OMCI_RC_PROCESSING);
		return;
	}
	bump_sync(m);
	omci_record_mutation(cl->apply, 1, 0, 0, sink);
	result_only(contents, OMCI_RC_SUCCESS);
}

static void dispatch(struct omci_mib *m, const struct omci_frame *req, struct omci_sink *sink,
		     uint8_t contents[OMCI_CONTENTS_LEN])
{
	switch (omci_action(req->message_type)) {
	case OMCI_MT_CREATE:
		do_create(m, req, sink, contents);
		break;
	case OMCI_MT_DELETE:
		do_delete(m, req, sink, contents);
		break;
	case OMCI_MT_SET:
		do_set(m, req, sink, contents);
		break;
	case OMCI_MT_GET:
		do_get(m, req, contents);
		break;
	case OMCI_MT_GET_NEXT:
		do_get_next(m, req, contents);
		break;
	case OMCI_MT_MIB_RESET:
		do_mib_reset(m, req, sink, contents);
		break;
	case OMCI_MT_MIB_UPLOAD:
		do_mib_upload(m, req, contents);
		break;
	case OMCI_MT_MIB_UPLOAD_NEXT:
		do_mib_upload_next(m, req, contents);
		break;
	case OMCI_MT_GET_ALL_ALARMS:
		do_get_all_alarms(m, req, contents);
		break;
	case OMCI_MT_GET_ALL_ALARMS_NEXT:
		result_only(contents, OMCI_RC_PARAMETER);
		break;
	case OMCI_MT_SYNC_TIME:
		do_sync_time(m, req, sink, contents);
		break;
	case OMCI_MT_START_SW_DOWNLOAD:
	case OMCI_MT_DOWNLOAD_SECTION:
	case OMCI_MT_END_SW_DOWNLOAD:
	case OMCI_MT_ACTIVATE_SW:
	case OMCI_MT_COMMIT_SW:
		if (req->class_id == 7)
			omci_swdl_handle(&m->swdl, omci_action(req->message_type), req->instance, 1,
					 req->contents, contents);
		else
			result_only(contents, OMCI_RC_NOT_SUPPORTED);
		break;
	case OMCI_MT_REBOOT:
	case OMCI_MT_TEST:
	case OMCI_MT_TEST_RESULT:
	case OMCI_MT_GET_CURRENT:
	case OMCI_MT_SET_TABLE:
		result_only(contents, OMCI_RC_NOT_SUPPORTED);
		break;
	default:
		result_only(contents, OMCI_RC_NOT_SUPPORTED);
		break;
	}
}

void omci_mib_set_swdl_dir(struct omci_mib *m, const char *dir)
{
	if (m)
		omci_swdl_init(&m->swdl, dir);
}

int omci_handle_frame(struct omci_mib *m, const uint8_t *raw, size_t len,
		      struct omci_sink *sink, const struct omci_integrity *ig,
		      uint8_t out[OMCI_FRAME_LEN])
{
	struct omci_frame req;
	struct omci_frame resp;

	if (!m || !raw || !out)
		return 0;
	if (len < 40 || raw[3] != OMCI_DEVICE_BASELINE)
		return 0;
	if (!ig || !ig->verify || !ig->verify(raw, len))
		return 0;
	if (!omci_decode(raw, len, &req))
		return 0;
	/* Sections without AR carry image data and get no response. */
	if (!omci_ack_request(req.message_type)) {
		if (omci_action(req.message_type) == OMCI_MT_DOWNLOAD_SECTION &&
		    req.class_id == 7)
			omci_swdl_handle(&m->swdl, OMCI_MT_DOWNLOAD_SECTION, req.instance, 0,
					 req.contents, resp.contents);
		return 0;
	}
	memset(&resp, 0, sizeof resp);
	resp.tci = req.tci;
	resp.message_type = omci_response_type(req.message_type);
	resp.device_id = OMCI_DEVICE_BASELINE;
	resp.class_id = req.class_id;
	resp.instance = req.instance;
	dispatch(m, &req, sink, resp.contents);
	omci_encode(&resp, ig, out);
	return 1;
}

/* Identity (G12b): writes the attribute into the baseline and the live MIB, so it
  * survives a MIB reset. 0 ok, -1 if the instance does not exist or the size does not
  * match. It does not go through OMCI Set: identity attributes are read-only
  * for the OLT. */
int omci_mib_set_base_attr(struct omci_mib *m, uint16_t class_id, uint16_t me_id,
			   unsigned index, const uint8_t *bytes, size_t n)
{
	const struct omci_class *cl = omci_class_by_id(class_id);
	struct omci_inst *in;
	uint16_t i;
	int found = 0;

	if (!m || !bytes || !cl || index >= cl->n_attrs)
		return -1;
	if (cl->attrs[index].kind == OMCI_KIND_TABLE || cl->attrs[index].size != n ||
	    n > OMCI_VALUE_MAX)
		return -1;
	in = find_mut(m, class_id, me_id);
	if (in) {
		memcpy(in->scalar[index], bytes, n);
		found = 1;
	}
	for (i = 0; i < m->n_base; i++) {
		struct omci_inst *b = &m->base[i];

		if (b->used && b->class_id == class_id && b->me_id == me_id) {
			memcpy(b->scalar[index], bytes, n);
			found = 1;
		}
	}
	return found ? 0 : -1;
}
