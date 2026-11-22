/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef OMCID_OMCI_H
#define OMCID_OMCI_H

#include <stddef.h>
#include <stdint.h>

#define OMCI_FRAME_LEN 48
#define OMCI_CONTENTS_LEN 32
#define OMCI_DEVICE_BASELINE 0x0A
#define OMCI_DEVICE_EXTENDED 0x0B
#define OMCI_AK 0x20
#define OMCI_AR 0x40
#define OMCI_DB 0x80
#define OMCI_MT_MASK 0x1F
#define OMCI_BASELINE_LENGTH 40u

#define OMCI_MT_CREATE 4
#define OMCI_MT_DELETE 6
#define OMCI_MT_SET 8
#define OMCI_MT_GET 9
#define OMCI_MT_GET_ALL_ALARMS 11
#define OMCI_MT_GET_ALL_ALARMS_NEXT 12
#define OMCI_MT_MIB_UPLOAD 13
#define OMCI_MT_MIB_UPLOAD_NEXT 14
#define OMCI_MT_MIB_RESET 15
#define OMCI_MT_ALARM 16
#define OMCI_MT_AVC 17
#define OMCI_MT_TEST 18
#define OMCI_MT_START_SW_DOWNLOAD 19
#define OMCI_MT_DOWNLOAD_SECTION 20
#define OMCI_MT_END_SW_DOWNLOAD 21
#define OMCI_MT_ACTIVATE_SW 22
#define OMCI_MT_COMMIT_SW 23
#define OMCI_MT_SYNC_TIME 24
#define OMCI_MT_REBOOT 25
#define OMCI_MT_GET_NEXT 26
#define OMCI_MT_TEST_RESULT 27
#define OMCI_MT_GET_CURRENT 28
#define OMCI_MT_SET_TABLE 29

#define OMCI_RC_SUCCESS 0
#define OMCI_RC_PROCESSING 1
#define OMCI_RC_NOT_SUPPORTED 2
#define OMCI_RC_PARAMETER 3
#define OMCI_RC_UNKNOWN_ME 4
#define OMCI_RC_UNKNOWN_INSTANCE 5
#define OMCI_RC_DEVICE_BUSY 6
#define OMCI_RC_INSTANCE_EXISTS 7
#define OMCI_RC_ATTRIBUTE_FAILURE 9

#define OMCI_GET_VALUE_BUDGET 25
#define OMCI_GET_NEXT_BUDGET 29
#define OMCI_UPLOAD_NEXT_BUDGET 26

/* Cap of gpe_omci_msg's message[]. ioctl_xport.c asserts it against the header. */
#define OMCI_RX_MAX 1980

#define OMCI_VALUE_MAX 28
#define OMCI_MAX_ATTR 16

#define OMCI_F_R 0x01
#define OMCI_F_W 0x02
#define OMCI_F_C 0x04
#define OMCI_F_NOUP 0x08
#define OMCI_F_NS 0x10

#define OMCI_KIND_INT 0
#define OMCI_KIND_OCTETS 1
#define OMCI_KIND_TABLE 2

#define OMCI_APPLY_NONE 0
#define OMCI_APPLY_BRIDGE 1
#define OMCI_APPLY_BRIDGE_PORT 2
#define OMCI_APPLY_VLAN_FILTER 3
#define OMCI_APPLY_PMAPPER 4
#define OMCI_APPLY_EXT_VLAN 5
#define OMCI_APPLY_GEM_IW 6
#define OMCI_APPLY_GEM_PORT 7
#define OMCI_APPLY_TCONT 8
#define OMCI_APPLY_QUEUE 9
#define OMCI_APPLY_ONU_G 10
#define OMCI_APPLY_ANI 11
#define OMCI_APPLY_LASER 12
#define OMCI_APPLY_STORE 13
#define OMCI_APPLY_DEBUG 14

#define OMCI_RESET_LINE \
	"MIB reset; would re-apply the internal voice port 47/259 (TP type 4, outside the upload); the laser is not touched"
#define OMCI_VOICE_PORT_LINE \
	"would apply BP4 LAN port 4 (internal state, outside the upload)"
#define OMCI_SYNC_LINE "would apply clock sync"

struct omci_attr {
	uint8_t size;
	uint8_t kind;
	uint8_t flags;
};

struct omci_class {
	uint16_t id;
	const char *name;
	const struct omci_attr *attrs;
	uint8_t n_attrs;
	uint8_t allow_create;
	uint8_t allow_delete;
	uint8_t sensitive;
	uint8_t apply;
};

struct omci_frame {
	uint16_t tci;
	uint8_t message_type;
	uint8_t device_id;
	uint16_t class_id;
	uint16_t instance;
	uint8_t contents[OMCI_CONTENTS_LEN];
};

struct omci_integrity {
	void (*seal)(uint8_t frame[OMCI_FRAME_LEN]);
	int (*verify)(const uint8_t *frame, size_t len);
};

struct omci_sink {
	void (*record)(void *ctx, const char *line);
	void *ctx;
};

/* 0 ok, 1 fin, -1 error. Nunca aborta. */
struct omci_transport {
	int (*recv)(struct omci_transport *t, uint8_t *buf, size_t cap, size_t *n);
	int (*send)(struct omci_transport *t, const uint8_t *frame, size_t n);
	void *ctx;
};

struct omci_mib;

static inline uint16_t omci_be16(const uint8_t *p)
{
	return (uint16_t)(((unsigned)p[0] << 8) | p[1]);
}

static inline void omci_put_be16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static inline uint32_t omci_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline void omci_put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static inline uint16_t omci_mask_bit(unsigned index)
{
	if (index >= 16)
		return 0;
	return (uint16_t)(1u << (15u - index));
}

static inline uint8_t omci_action(uint8_t mt)
{
	return (uint8_t)(mt & OMCI_MT_MASK);
}

static inline int omci_ack_request(uint8_t mt)
{
	return (mt & OMCI_AR) != 0;
}

const struct omci_class *omci_class_by_id(uint16_t id);
const struct omci_class *omci_classes(size_t *n);
int omci_attr_readable(const struct omci_attr *a);
int omci_attr_writable(const struct omci_attr *a);
int omci_attr_set_by_create(const struct omci_attr *a);
int omci_attr_uploadable(const struct omci_attr *a);
int omci_classes_consistent(void);

int omci_decode(const uint8_t *raw, size_t len, struct omci_frame *out);
void omci_encode(const struct omci_frame *frame, const struct omci_integrity *ig,
		 uint8_t out[OMCI_FRAME_LEN]);
uint8_t omci_response_type(uint8_t mt);

uint32_t omci_aal5_crc32(const uint8_t *data, size_t len);
extern const struct omci_integrity omci_passthrough;
extern const struct omci_integrity omci_aal5;

void omci_record_mutation(uint8_t apply, int deleting, int touched_admin,
			  int touched_alloc, struct omci_sink *sink);

/* Software download in dump mode (swdl.c): never applied. */
struct omci_swdl {
	char dir[160];
	char path[200];
	int fd;
	uint16_t inst;
	uint32_t size, base, sections, started;
	unsigned window;
};
void omci_swdl_init(struct omci_swdl *s, const char *dir);
int omci_swdl_handle(struct omci_swdl *s, uint8_t action, uint16_t inst, int ack,
		     const uint8_t *contents, uint8_t *out);

struct omci_mib *omci_mib_new(void);
/* dir != NULL: accept and dump the OLT's software downloads there. */
void omci_mib_set_swdl_dir(struct omci_mib *m, const char *dir);
void omci_mib_free(struct omci_mib *m);
int omci_mib_parse(struct omci_mib *m, const char *text, char *err, size_t errlen);
int omci_mib_load_path(struct omci_mib *m, const char *path, char *err, size_t errlen);
size_t omci_mib_len(const struct omci_mib *m);
int omci_mib_contains(const struct omci_mib *m, uint16_t class_id, uint16_t me_id);
int omci_mib_at(const struct omci_mib *m, size_t index, uint16_t *class_id, uint16_t *me_id);
const uint8_t *omci_mib_scalar(const struct omci_mib *m, uint16_t class_id, uint16_t me_id,
			       unsigned index, size_t *len);
int omci_mib_table(const struct omci_mib *m, uint16_t class_id, uint16_t me_id,
		   unsigned index, uint8_t *dst, size_t cap);
int omci_mib_sync(const struct omci_mib *m, uint8_t *out);
int omci_mib_clock(const struct omci_mib *m, uint8_t out[7]);
int omci_mib_voice_noted(const struct omci_mib *m);

/* Apply hook (G12a, gpe.c). Called AFTER mutating the MIB and returns an OMCI
  * result code (0 = success). If it returns non-zero on Create/Set,
  * the MIB undoes the change and answers that code. On Delete it restores the instance.
  * On reset the value is ignored (the hook retries on the next event). */
#define OMCI_HOOK_CREATE 1
#define OMCI_HOOK_SET 2
#define OMCI_HOOK_DELETE 3
#define OMCI_HOOK_RESET 4
struct omci_hook {
	int (*event)(void *ctx, const struct omci_mib *m, int ev, uint16_t class_id,
		     uint16_t me_id);
	void *ctx;
};
void omci_mib_set_hook(struct omci_mib *m, const struct omci_hook *hook);

/* 1 if there is a response in out, 0 if it is discarded. Never aborts. */
int omci_handle_frame(struct omci_mib *m, const uint8_t *raw, size_t len,
		      struct omci_sink *sink, const struct omci_integrity *ig,
		      uint8_t out[OMCI_FRAME_LEN]);

int omci_serve(struct omci_mib *m, struct omci_transport *transport,
	       struct omci_sink *sink, const struct omci_integrity *ig);

int omci_file_transport_open(struct omci_transport *t, const char *in_path,
			     const char *out_path, char *err, size_t errlen);
void omci_file_transport_close(struct omci_transport *t);
void omci_mem_transport_init(struct omci_transport *t, const uint8_t *in, size_t in_len,
			     uint8_t *out, size_t out_cap, size_t *out_len);
void omci_mem_transport_close(struct omci_transport *t);

/* The only path that opens a device. The tests do not call it. */
int omci_ioctl_transport_open(struct omci_transport *t, const char *path,
			      char *err, size_t errlen);
void omci_ioctl_transport_close(struct omci_transport *t);

/* Entrada del binario. --frames puede llevar --gpe-apply. */
int omcid_main(int argc, char **argv);

#endif
