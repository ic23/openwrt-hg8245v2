/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HG8245_SLICPROBE_EMBEDDED_H
#define HG8245_SLICPROBE_EMBEDDED_H

/* One stock-style LE88226 ID read; the caller must isolate TEL first. */
int hg8245_codec_read_id(void);

#endif
