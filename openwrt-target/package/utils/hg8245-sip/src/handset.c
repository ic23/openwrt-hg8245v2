/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <pjsua-lib/pjsua.h>
#include <pjmedia/alaw_ulaw.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/file.h>
#include <unistd.h>

#define SLIC "/sys/bus/spi/devices/spi0.0/"
#define SAMPLES 80
static int audio_fd, tel_fd;
static volatile sig_atomic_t quit;
static pjsua_call_id active = PJSUA_INVALID_ID;
static pjsua_conf_port_id handset_slot;
static pjmedia_port handset;
static unsigned long rx_frames, tx_frames, rx_missed, tx_missed;
static int ringing;
static time_t alert_end;

static void terminated(int sig) { quit = 1; }

static int command(int fd, const char *cmd)
{
	size_t n = strlen(cmd);
	if (pwrite(fd, cmd, n, 0) == (ssize_t)n)
		return 0;
	perror(cmd);
	return -1;
}

static int off_hook(void)
{
	char state[64];
	ssize_t n = pread(tel_fd, state, sizeof(state) - 1, 0);
	if (n <= 0)
		return -1;
	state[n] = 0;
	return strstr(state, "off-hook") != NULL;
}

static pj_status_t capture(pjmedia_port *p, pjmedia_frame *f)
{
	unsigned char block[SAMPLES];
	short *samples = f->buf;
	if (pread(audio_fd, block, sizeof(block), 0) == sizeof(block)) {
		for (unsigned i = 0; i < SAMPLES; i++)
			samples[i] = pjmedia_alaw2linear(block[i]);
		rx_frames++;
	} else {
		memset(samples, 0, SAMPLES * sizeof(*samples));
		rx_missed++;
	}
	f->size = SAMPLES * sizeof(*samples);
	f->type = PJMEDIA_FRAME_TYPE_AUDIO;
	return PJ_SUCCESS;
}

static pj_status_t playback(pjmedia_port *p, pjmedia_frame *f)
{
	unsigned char block[SAMPLES];
	const short *samples = f->buf;
	if (f->type != PJMEDIA_FRAME_TYPE_AUDIO || f->size != sizeof(short) * SAMPLES)
		return PJ_SUCCESS;
	for (unsigned i = 0; i < SAMPLES; i++)
		block[i] = pjmedia_linear2alaw(samples[i]);
	if (pwrite(audio_fd, block, sizeof(block), 0) == sizeof(block))
		tx_frames++;
	else
		tx_missed++;
	return PJ_SUCCESS;
}

static void incoming(pjsua_acc_id acc, pjsua_call_id id, pjsip_rx_data *msg)
{
	if (active != PJSUA_INVALID_ID || off_hook() != 0 || command(tel_fd, "incoming")) {
		pjsua_call_answer(id, 486, NULL, NULL);
		return;
	}
	active = id;
	ringing = 1;
	alert_end = time(NULL) + 55;
	pjsua_call_answer(id, 180, NULL, NULL);
}

static void call_state(pjsua_call_id id, pjsip_event *event)
{
	pjsua_call_info info;
	if (pjsua_call_get_info(id, &info) != PJ_SUCCESS || id != active)
		return;
	if (info.state == PJSIP_INV_STATE_DISCONNECTED) {
		active = PJSUA_INVALID_ID;
		ringing = 0;
		command(tel_fd, off_hook() > 0 ? "busy" : "phone");
	} else if (info.role == PJSIP_ROLE_UAC && info.state == PJSIP_INV_STATE_EARLY) {
		command(tel_fd, "ringback");
	}
}

static void media_state(pjsua_call_id id)
{
	pjsua_call_info ci;
	if (pjsua_call_get_info(id, &ci) != PJ_SUCCESS || ci.media_status != PJSUA_CALL_MEDIA_ACTIVE)
		return;
	if (command(tel_fd, "talk") ||
	    pjsua_conf_connect(ci.conf_slot, handset_slot) != PJ_SUCCESS ||
	    pjsua_conf_connect(handset_slot, ci.conf_slot) != PJ_SUCCESS)
		pjsua_call_hangup(id, 500, NULL, NULL);
}

static pj_status_t release_handset(pjmedia_port *p)
{
	pj_pool_release(p->port_data.pdata);
	return PJ_SUCCESS;
}

static pj_status_t endpoint(char *address, pjsua_acc_id *acc)
{
	pjsua_config ua;
	pjsua_media_config media;
	pjsua_logging_config log;
	pjsua_transport_config net;
	pjsua_acc_config account;
	pjsua_transport_id transport;
	pj_pool_t *pool;
	pj_str_t name = pj_str("HG8245 handset");
	char identity[96];
	pj_status_t rc;

	pjsua_config_default(&ua);
	ua.thread_cnt = 0;
	ua.max_calls = 1;
	ua.cb.on_incoming_call = incoming;
	ua.cb.on_call_state = call_state;
	ua.cb.on_call_media_state = media_state;
	pjsua_logging_config_default(&log);
	log.console_level = 3;
	pjsua_media_config_default(&media);
	media.clock_rate = media.snd_clock_rate = 8000;
	media.audio_frame_ptime = 10;
	media.ptime = 20;
	media.channel_count = 1;
	media.ec_tail_len = 0;
	media.no_vad = PJ_TRUE;
	media.has_ioqueue = PJ_FALSE;
	media.thread_cnt = 0;
	media.max_media_ports = 6;
	rc = pjsua_init(&ua, &log, &media);
	if (rc) return rc;
	pjsua_transport_config_default(&net);
	net.port = 5060;
	net.bound_addr = net.public_addr = pj_str(address);
	rc = pjsua_transport_create(PJSIP_TRANSPORT_UDP, &net, &transport);
	if (rc) return rc;
	rc = pjsua_start();
	if (rc) return rc;
	pjsua_acc_config_default(&account);
	snprintf(identity, sizeof(identity), "sip:phone@%s", address);
	account.id = pj_str(identity);
	account.transport_id = transport;
	account.rtp_cfg.port = 4000;
	account.rtp_cfg.bound_addr = account.rtp_cfg.public_addr = net.bound_addr;
	rc = pjsua_acc_add(&account, PJ_TRUE, acc);
	if (rc) return rc;
	rc = pjsua_set_null_snd_dev();
	if (rc) return rc;
	pool = pjsua_pool_create("handset", 2048, 2048);
	if (!pool) return PJ_ENOMEM;
	pjmedia_port_info_init(&handset.info, &name, PJMEDIA_SIG_CLASS_PORT_AUD('F','X'), 8000, 1, 16, SAMPLES);
	handset.get_frame = capture;
	handset.put_frame = playback;
	handset.port_data.pdata = pool;
	handset.on_destroy = release_handset;
	rc = pjmedia_port_init_grp_lock(&handset, pool, NULL);
	if (rc) {
		pj_pool_release(pool);
		return rc;
	}
	rc = pjsua_conf_add_port(pool, &handset, &handset_slot);
	pjmedia_port_dec_ref(&handset);
	return rc;
}

int main(int argc, char **argv)
{
	char path[128];
	struct in_addr address;
	pjsua_acc_id acc;
	int previous, hook, rc = 1, service, lock;
	pj_status_t status;
	if ((argc != 3 && argc != 4) || (strcmp(argv[1], "tel1") && strcmp(argv[1], "tel2"))
	    || inet_pton(AF_INET, argv[2], &address) != 1 || address.s_addr == INADDR_ANY) {
		fprintf(stderr, "Usage: hg8245-sip tel1|tel2 LAN_IP [sip:peer@address]\n");
		return 2;
	}
	lock = open("/var/run/hg8245-sip.lock", O_CREAT | O_RDWR, 0600);
	if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) { perror("hg8245-sip lock"); return 1; }
	snprintf(path, sizeof(path), SLIC "%s_audio", argv[1]);
	audio_fd = open(path, O_RDWR);
	snprintf(path, sizeof(path), SLIC "%s", argv[1]);
	tel_fd = open(path, O_RDWR);
	service = open(SLIC "service", O_RDWR);
	if (tel_fd < 0 || audio_fd < 0 || service < 0) { perror("FXS"); return 1; }
	signal(SIGINT, terminated);
	signal(SIGTERM, terminated);
	if (command(service, "media")) return 1;
	if (command(tel_fd, "phone")) goto stop;
	status = pjsua_create();
	if (status) goto stop;
	status = endpoint(argv[2], &acc);
	if (status || (argc == 4 && (status = pjsua_verify_url(argv[3])))) goto done;
	printf("Listening on sip:phone@%s:5060 (UDP), %s\n", argv[2], argv[1]);
	fflush(stdout);
	previous = off_hook();
	while (!quit && previous >= 0) {
		pjsua_handle_events(30);
		hook = off_hook();
		if (hook < 0) break;
		if (ringing && hook) {
			ringing = 0;
			pjsua_call_answer(active, 200, NULL, NULL);
		} else if (ringing && time(NULL) >= alert_end) {
			ringing = 0;
			pjsua_call_hangup(active, 480, NULL, NULL);
		} else if (previous && !hook && active != PJSUA_INVALID_ID) {
			pjsua_call_hangup(active, 0, NULL, NULL);
		} else if (!previous && hook && active == PJSUA_INVALID_ID && argc == 4) {
			pj_str_t uri = pj_str(argv[3]);
			if (pjsua_call_make_call(acc, &uri, 0, NULL, NULL, &active) != PJ_SUCCESS)
				command(tel_fd, "busy");
		}
		previous = hook;
	}
	rc = quit ? 0 : 1;
done:
	if (status) pjsua_perror("hg8245-sip", "SIP setup", status);
	pjsua_destroy();
	printf("DSP 10ms frames: mic=%lu speaker=%lu missed_mic=%lu missed_speaker=%lu\n",
	       rx_frames, tx_frames, rx_missed, tx_missed);
stop:
	command(service, "off");
	return rc;
}
