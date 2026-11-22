/* SPDX-License-Identifier: GPL-2.0-only */
#include "service.h"
#include "identity.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#define DEVICE "/sys/bus/platform/devices/420000a0.gpon-rx"
#define INVENTORY "/usr/share/sd5113-omcid/board.mib"

static volatile sig_atomic_t stopping;
static void stop(int sig) { (void)sig; stopping = 1; }

static int attr(const char *name, void *buf, size_t len, int writing)
{
	int fd = open(name, (writing ? O_WRONLY : O_RDONLY) | O_CLOEXEC);
	int n, saved;
	if (fd < 0)
		return -errno;
	n = writing ? write(fd, buf, len) : read(fd, buf, len);
	saved = errno;
	close(fd);
	if (n < 0)
		return -saved;
	return writing && (size_t)n != len ? -EIO : n;
}

static int configure(void *ctx, const struct omci_mib *m, int ev, uint16_t cls, uint16_t id)
{
	struct sd5113_service svc;
	char cmd[320];
	int p, used;
	int n, rc;
	(void)ev; (void)cls; (void)id;
	rc = sd5113_service(m, &svc);
	if (rc)
		return rc;
	used = snprintf(cmd, sizeof(cmd), "map %u %d %d %d ", *(unsigned *)ctx,
			svc.vid, svc.tag_pcp, svc.unmarked);
	for (p = 0; p < (int)sizeof(svc.dscp); p++)
		used += snprintf(cmd + used, sizeof(cmd) - used, "%02x", svc.dscp[p]);
	for (p = 0; p < SD5113_PBITS; p++)
		used += snprintf(cmd + used, sizeof(cmd) - used, " %08x", svc.paths[p]);
	cmd[used++] = ' ';
	for (p = 0; p < 2; p++)
		for (unsigned i = 0; i < sizeof(svc.filters[p]); i++)
			used += snprintf(cmd + used, sizeof(cmd) - used, "%02x", svc.filters[p][i]);
	n = attr("gem_service", cmd, strlen(cmd), 1);
	return n >= 0 ? OMCI_RC_SUCCESS : n == -EAGAIN ? OMCI_RC_DEVICE_BUSY : OMCI_RC_PROCESSING;
}

static struct omci_mib *inventory(const struct omci_hook *hook)
{
	struct omci_mib *m = omci_mib_new();
	char serial[18] = {0}, err[192] = {0};
	uint8_t sn[8];
	unsigned i;
	if (!m || omci_mib_load_path(m, INVENTORY, err, sizeof(err)) ||
	    attr("onu_serial", serial, sizeof(serial) - 1, 0) < 16)
		goto fail;
	for (i = 0; i < sizeof(sn); i++) {
		unsigned v;
		if (sscanf(serial + 2*i, "%2x", &v) != 1)
			goto fail;
		sn[i] = v;
	}
	if (omci_mib_set_base_attr(m, 256, 0, 0, sn, 4) ||
	    omci_mib_set_base_attr(m, 256, 0, 2, sn, 8))
		goto fail;
	omci_mib_set_hook(m, hook);
	return m;
fail:
	fprintf(stderr, "cannot initialize board MIB: %s\n", err);
	omci_mib_free(m);
	return NULL;
}

struct transaction {
	int valid;
	uint8_t request[40], response[OMCI_FRAME_LEN];
};

static int session(unsigned *epoch, int *omcc)
{
	char line[48] = {0};
	return attr("omci_session", line, sizeof(line)-1, 0) > 0 &&
	       sscanf(line, "%u %d", epoch, omcc) == 2;
}

int main(void)
{
	unsigned epoch = 0, observed;
	struct omci_hook hook = {configure, &epoch};
	struct omci_mib *m = NULL;
	struct transaction cache[2] = {0};
	uint8_t rx[256], tx[44], response[OMCI_FRAME_LEN];
	int omcc;
	time_t last_sync = 0, last_error = 0;
	signal(SIGTERM, stop);
	signal(SIGINT, stop);
	if (chdir(DEVICE) || !omci_classes_consistent())
		return 1;
	if (attr("gem_service", "off", 3, 1) < 0 || attr("omci_capture", "1", 1, 1) < 0)
		return 1;
	while (!stopping) {
		int rc, i;
		struct transaction *t;
		if (!session(&observed, &omcc))
			break;
		if (!m || observed != epoch || omcc < 0) {
			if (m) {
				omci_mib_free(m);
				m = NULL;
			}
			if (omcc < 0) {
				usleep(250000);
				continue;
			}
			epoch = observed;
			memset(cache, 0, sizeof(cache));
			m = inventory(&hook);
			if (!m)
				break;
		}
		/* Alloc-ID can be withdrawn and reassigned by PLOAM without an
		 * OMCI transaction. Restore the committed profile after regrant. */
		{
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			if (last_sync != now.tv_sec) {
				last_sync = now.tv_sec;
				configure(&epoch, m, 0, 0, 0);
			}
		}
		rc = attr("omci_frame", rx, sizeof(rx), 0);
		if (rc == -EAGAIN) {
			usleep(3000);
			continue;
		}
		if (rc != sizeof(rx))
			break;
		if (!session(&observed, &omcc) || observed != epoch || omcc < 0)
			continue;
		t = &cache[rx[0] >> 7];
		if (t->valid && !memcmp(t->request, rx, sizeof(t->request))) {
			memcpy(response, t->response, sizeof(response));
		} else {
			if (!omci_handle_frame(m, rx, 40, NULL, &omci_passthrough, response))
				continue;
			if ((omci_action(rx[2]) == OMCI_MT_CREATE ||
			     omci_action(rx[2]) == OMCI_MT_SET ||
			     omci_action(rx[2]) == OMCI_MT_DELETE) &&
			    response[8] && last_sync != last_error) {
				fprintf(stderr, "OMCI operation %u class %u instance %u result %u\n",
					omci_action(rx[2]), omci_be16(rx+4), omci_be16(rx+6), response[8]);
				last_error = last_sync;
			}
			memcpy(t->request, rx, sizeof(t->request));
			memcpy(t->response, response, sizeof(response));
			t->valid = response[8] != OMCI_RC_DEVICE_BUSY;
		}
		omci_put_be32(tx, epoch);
		memcpy(tx + 4, response, 40);
		for (i = 0; i < 50 && !stopping; i++) {
			rc = attr("omci_tx", tx, sizeof(tx), 1);
			if (rc != -EAGAIN)
				break;
			usleep(2000);
		}
	}
	attr("gem_service", "off", 3, 1);
	attr("omci_capture", "0", 1, 1);
	omci_mib_free(m);
	return stopping ? 0 : 1;
}
