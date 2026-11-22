// SPDX-License-Identifier: GPL-2.0-only
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/spi/spi.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#undef TRUE
#undef FALSE
#include "vp_api.h"
#include "vp_hal.h"
#include "profiles.h"
#include "line-tones.h"
#include "zsp-service.h"

enum call_mode { DIRECT, PHONE, BUSY, RINGBACK, ALERT, ANSWERED, BRIDGED };
static const char * const calls[] = {
	[PHONE] = "phone", [BUSY] = "busy", [RINGBACK] = "ringback",
	[ALERT] = "incoming", [ANSWERED] = "answered", [BRIDGED] = "local",
};

struct le88266 {
	struct spi_device *spi;
	struct mutex lock;
	struct task_struct *poller;
	wait_queue_head_t wait;
	struct zsp_service voice;
	zarlink_device_id_t id;
	VpDevCtxType ctx;
	Vp880DeviceObjectType devobj;
	VpLineCtxType lines[2];
	Vp880LineObjectType lineobj[2];
	int io_error;
	u8 identity[2];
	bool identified;
	bool ports_swapped;
	bool intercom;
	u8 hooks;
	bool pending_audio[2];
	u8 audio[2][80];
	bool media;
	bool running;
	enum call_mode call[2];
	unsigned long next_phase[2];
	unsigned long alert_end[2];
	bool phase_on[2];
	bool fault;
	unsigned int streams;
	VpOptionCodecType law;
	struct spi_transfer transfers[17];
	u8 bytes[17];
};

/* The factory maps the DSP in jack order. VoicePath addresses SLIC channels. */
static unsigned int dsp_channel(struct le88266 *s, unsigned int slic)
{
	return slic ^ s->ports_swapped;
}

static const char *tel_name(struct le88266 *s, unsigned int ch)
{
	return dsp_channel(s, ch) ? "tel2" : "tel1";
}

static unsigned int tel_channel(struct le88266 *s, struct device_attribute *attr)
{
	return (attr->attr.name[3] - '1') ^ s->ports_swapped;
}

/* RUN ends CS after each byte. The controller accepts 17 byte-transfers per message. */
static int mpi_command(struct le88266 *s, u8 cmd, unsigned int len, u8 *data)
{
	unsigned int pos = 0, total = len + 1;
	bool read = cmd & 1;

	while (pos < total) {
		unsigned int count = min_t(unsigned int, total - pos, ARRAY_SIZE(s->bytes));
		struct spi_message message;
		unsigned int i;
		int err;

		memset(s->transfers, 0, count * sizeof(*s->transfers));
		for (i = 0; i < count; i++) {
			struct spi_transfer *x = &s->transfers[i];
			unsigned int byte = pos + i;

			x->len = 1;
			if (byte && read)
				x->rx_buf = s->bytes + i;
			else {
				s->bytes[i] = byte ? data[byte - 1] : cmd;
				x->tx_buf = s->bytes + i;
			}
			if (i + 1 < count) {
				x->cs_change = 1;
				x->cs_change_delay.unit = SPI_DELAY_UNIT_NSECS;
			}
			if (byte + 1 < total) {
				x->delay.unit = SPI_DELAY_UNIT_USECS;
				x->delay.value = read ? 50 : 75;
			}
		}
		spi_message_init_with_transfers(&message, s->transfers, count);
		err = spi_sync_locked(s->spi, &message);
		if (err || message.actual_length != count)
			return err ?: -EIO;
		if (read && len) {
			unsigned int skip = !pos;

			memcpy(data + pos + skip - 1, s->bytes + skip, count - skip);
		}
		pos += count;
	}
	return 0;
}

static int mpi_transfer(struct le88266 *s, u8 *ec, u8 cmd, u8 len, u8 *data)
{
	int err;

	lockdep_assert_held(&s->lock);
	err = spi_bus_lock(s->spi->controller);
	if (err)
		return err;
	if (ec)
		err = mpi_command(s, 0x4a, 1, ec);
	if (!err)
		err = mpi_command(s, cmd, len, data);
	spi_bus_unlock(s->spi->controller);
	return err;
}

void VpMpiCmd(VpDeviceIdType id, uint8 ec, uint8 cmd, uint8 len, uint8 *data)
{
	struct le88266 *s = id->mpi;

	if (!s->io_error)
		s->io_error = mpi_transfer(s, cmd == 0x4b ? NULL : &ec, cmd, len, data);
	if (s->io_error && (cmd & 1) && len)
		memset(data, 0xff, len);
}

/* All API calls, including the periodic tick, run under the per-chip mutex. */
uint8 VpSysEnterCritical(VpDeviceIdType id, VpCriticalSecType type)
{
	lockdep_assert_held(&id->mpi->lock);
	return 0;
}

uint8 VpSysExitCritical(VpDeviceIdType id, VpCriticalSecType type)
{
	lockdep_assert_held(&id->mpi->lock);
	return 0;
}

void VpSysWait(uint8 ticks)
{
	if (ticks)
		usleep_range(ticks * 125, ticks * 125 + 100);
}

static int vp_result(struct le88266 *s, VpStatusType status)
{
	if (status != VP_STATUS_SUCCESS)
		dev_err(&s->spi->dev, "VoicePath status %d\n", status);
	return s->io_error ?: (status == VP_STATUS_SUCCESS ? 0 : -EIO);
}

/* Hook supervision and dial tone are local SLIC services, independent of WAN. */
static int phone_hook(struct le88266 *s, unsigned int ch, bool offhook)
{
	VpLineCtxType *line = &s->lines[ch];
	int ret;

	if (offhook) {
		ret = vp_result(s, VpSetLineState(line, VP_LINE_TALK));
		return ret ?: vp_result(s, VpSetLineTone(line, TONE_DIAL, NULL, NULL));
	}
	ret = vp_result(s, VpSetLineTone(line, NULL, NULL, NULL));
	return ret ?: vp_result(s, VpSetLineState(line, VP_LINE_STANDBY));
}

static int set_phase(struct le88266 *s, unsigned int ch, bool on)
{
	enum call_mode mode = s->call[ch];
	unsigned int duration;
	int ret;

	if (mode == ALERT) {
		ret = vp_result(s, VpSetLineState(&s->lines[ch],
					on ? VP_LINE_RINGING : VP_LINE_STANDBY));
	} else {
		VpProfilePtrType tone = mode == BUSY ? TONE_BUSY : TONE_RINGBACK;

		ret = vp_result(s, VpSetLineTone(&s->lines[ch], on ? tone : NULL, NULL, NULL));
	}
	duration = mode == BUSY ? 500 : on ? 2000 : 4000;
	s->phase_on[ch] = on;
	s->next_phase[ch] = jiffies + msecs_to_jiffies(duration);
	return ret;
}

/* Both physical lines belong to one local call while intercom is active. */
static int intercom_line(struct le88266 *s, unsigned int ch, enum call_mode mode)
{
	VpLineCtxType *line = &s->lines[ch];
	int ret;

	if (s->call[ch] == mode)
		return 0;
	s->call[ch] = mode;
	ret = vp_result(s, VpSetLineTone(line, NULL, NULL, NULL));
	if (!ret && mode != ALERT)
		ret = vp_result(s, VpSetLineState(line,
				mode == BRIDGED ? VP_LINE_STANDBY : VP_LINE_TALK));
	if (!ret && (mode == ALERT || mode == RINGBACK || mode == BUSY)) {
		if (mode == ALERT)
			s->alert_end[ch] = jiffies + msecs_to_jiffies(60000);
		ret = set_phase(s, ch, true);
	}
	sysfs_notify(&s->spi->dev.kobj, NULL, tel_name(s, ch));
	return ret;
}

static int intercom_hooks(struct le88266 *s)
{
	unsigned int ch, caller;
	u8 hooks = 0, previous = s->hooks;
	int ret;

	for (ch = 0; ch < 2; ch++) {
		bool offhook;

		ret = vp_result(s, VpGetLineStatus(&s->lines[ch], VP_INPUT_HOOK, &offhook));
		if (ret)
			return ret;
		if (offhook)
			hooks |= BIT(ch);
	}
	if (previous == hooks)
		return 0;
	s->hooks = hooks;
	memset(s->pending_audio, 0, sizeof(s->pending_audio));
	if (!hooks)
		return intercom_line(s, 0, BRIDGED) ?: intercom_line(s, 1, BRIDGED);
	if (hooks == 3) {
		ret = intercom_line(s, 0, ANSWERED) ?: intercom_line(s, 1, ANSWERED);
		return ret ?: zsp_service_flush_capture(&s->voice);
	}
	caller = hooks == BIT(0) ? 0 : 1;
	if (previous == 3)
		return intercom_line(s, caller ^ 1, BRIDGED) ?: intercom_line(s, caller, BUSY);
	return intercom_line(s, caller, RINGBACK) ?: intercom_line(s, caller ^ 1, ALERT);
}

static int intercom_audio(struct le88266 *s)
{
	unsigned int ch, i;
	int ret;

	if (s->hooks != 3)
		return 0;
	for (ch = 0; ch < 2; ch++) {
		for (i = 0; i < 3; i++) {
			if (!s->pending_audio[ch]) {
				ret = zsp_service_audio(&s->voice, dsp_channel(s, ch), false, s->audio[ch]);
				if (ret == -EAGAIN)
					break;
				if (ret == -ENODATA)
					continue;
				if (ret)
					return ret;
				s->pending_audio[ch] = true;
			}
			ret = zsp_service_audio(&s->voice, dsp_channel(s, ch ^ 1), true, s->audio[ch]);
			if (ret == -EAGAIN)
				break;
			if (ret)
				return ret;
			s->pending_audio[ch] = false;
		}
	}
	return 0;
}

static int call_hook(struct le88266 *s, unsigned int ch, bool offhook)
{
	switch (s->call[ch]) {
	case DIRECT:
		return 0;
	case ALERT:
		if (!offhook)
			return 0;
		s->call[ch] = ANSWERED;
		return vp_result(s, VpSetLineState(&s->lines[ch], VP_LINE_TALK));
	case BRIDGED:
		return vp_result(s, VpSetLineState(&s->lines[ch],
					offhook ? VP_LINE_TALK : VP_LINE_STANDBY));
	case ANSWERED:
	case BUSY:
	case RINGBACK:
		if (offhook)
			return 0;
		s->call[ch] = PHONE;
		fallthrough;
	case PHONE:
		return phone_hook(s, ch, offhook);
	}
	return -EINVAL;
}

static int advance_calls(struct le88266 *s)
{
	unsigned int ch;

	for (ch = 0; ch < ARRAY_SIZE(s->lines); ch++) {
		int ret;

		if (s->call[ch] != ALERT && s->call[ch] != BUSY && s->call[ch] != RINGBACK)
			continue;
		if (s->call[ch] == ALERT && time_after_eq(jiffies, s->alert_end[ch])) {
			if (s->intercom)
				return intercom_line(s, ch, BRIDGED) ?: intercom_line(s, ch ^ 1, BUSY);
			s->call[ch] = PHONE;
			ret = phone_hook(s, ch, false);
			sysfs_notify(&s->spi->dev.kobj, NULL, tel_name(s, ch));
		} else if (time_after_eq(jiffies, s->next_phase[ch])) {
			ret = set_phase(s, ch, !s->phase_on[ch]);
		} else {
			continue;
		}
		if (ret)
			return ret;
	}
	return 0;
}

static int tick(struct le88266 *s, unsigned int wanted, VpLineCtxType *line)
{
	VpEventType event;
	bool pending;
	int err;

	err = vp_result(s, VpApiTick(&s->ctx, &pending));
	if (err)
		return err;
	while (pending && VpGetEvent(&s->ctx, &event)) {
		if (!s->intercom && event.eventCategory == VP_EVCAT_SIGNALING && event.channelId < 2 &&
		    (event.eventId == VP_LINE_EVID_HOOK_OFF || event.eventId == VP_LINE_EVID_HOOK_ON)) {
			err = call_hook(s, event.channelId, event.eventId == VP_LINE_EVID_HOOK_OFF);
			if (err)
				return err;
			sysfs_notify(&s->spi->dev.kobj, NULL, tel_name(s, event.channelId));
		}
		if (event.status != VP_STATUS_SUCCESS ||
		    (event.eventCategory == VP_EVCAT_FAULT && event.eventData) ||
		    (event.eventCategory == VP_EVCAT_RESPONSE &&
		     event.eventId == VP_EVID_CAL_CMP && event.eventData == VP_CAL_FAILURE)) {
			dev_err(&s->spi->dev, "VoicePath event category %u id %#x data %#x status %d\n",
				event.eventCategory, event.eventId, event.eventData, event.status);
			return -EIO;
		}
		if (wanted && event.eventCategory == VP_EVCAT_RESPONSE &&
		    event.eventId == wanted && event.pLineCtx == line)
			return s->io_error ?: 1;
	}
	return s->io_error;
}

static int wait_event_id(struct le88266 *s, unsigned int event, VpLineCtxType *line)
{
	unsigned long until = jiffies + msecs_to_jiffies(10000);
	int ret;

	do {
		usleep_range(5000, 5500); /* The board device profile specifies 5 ms. */
		ret = tick(s, event, line);
		if (ret)
			return ret < 0 ? ret : 0;
	} while (time_before(jiffies, until));
	dev_err(&s->spi->dev, "VoicePath timeout waiting for event %#x (%s)\n",
		event, line ? tel_name(s, line - s->lines) : "device");
	return -ETIMEDOUT;
}

static int stop(struct le88266 *s)
{
	int err = 0;

	s->running = false;
	s->intercom = false;
	s->media = false;
	memset(s->call, 0, sizeof(s->call));
	if (s->identified) {
		s->io_error = 0;
		VpMpiCmd(&s->id, 3, 0x04, 0, NULL);
		err = s->io_error;
		if (!err)
			s->identified = false;
	}
	err = zsp_service_stop(&s->voice) ?: err;
	s->fault = !!err;
	return err;
}

static int read_identity(struct le88266 *s)
{
	/* RCN/PCN is global. The board's chip_diagnose sends only 0x73. */
	int err = mpi_transfer(s, NULL, 0x73, sizeof(s->identity), s->identity);

	if (err)
		dev_err(&s->spi->dev, "MPI identity transfer failed: %d\n", err);
	return err;
}

static int identify_for_init(struct le88266 *s)
{
	u8 sync[16];
	int err;

	s->io_error = 0;
	err = read_identity(s);
	if (err || s->identity[1] == 0xb3)
		return err;

	/* Recover an unsynchronised command parser before cold initialization. */
	memset(sync, 0x06, sizeof(sync));
	VpMpiCmd(&s->id, 1, 0x06, sizeof(sync), sync);
	VpMpiCmd(&s->id, 1, 0x73, sizeof(s->identity), s->identity);
	if (s->io_error)
		dev_err(&s->spi->dev, "MPI identity transfer failed: %d\n", s->io_error);
	return s->io_error;
}

static int configure_pcm(struct le88266 *s, bool dsp)
{
	/* The stock DSP bus uses a 16-bit word per channel. The SLIC's
	 * slot registers count bytes. PCMA is the DSP packet format.
	 */
	VpOptionCodecType codec = dsp ? VP_OPTION_WIDEBAND : VP_OPTION_ALAW;
	unsigned int ch;
	int ret;

	for (ch = 0; ch < ARRAY_SIZE(s->lines); ch++) {
		u8 slot = dsp ? 2 * dsp_channel(s, ch) : ch;
		VpOptionTimeslotType slots = { .tx = slot, .rx = slot };

		ret = vp_result(s, VpSetOption(&s->lines[ch], NULL, VP_OPTION_ID_TIMESLOT, &slots));
		if (ret)
			return ret;
	}

	/* Bandwidth is shared by the entire chip. Both lines have completed
	 * calibration and are disconnected before changing it.
	 */
	for (ch = 0; ch < ARRAY_SIZE(s->lines); ch++) {
		ret = vp_result(s, VpSetOption(&s->lines[ch], NULL, VP_OPTION_ID_CODEC, &codec));
		if (ret)
			return ret;
	}
	return 0;
}

static int start(struct le88266 *s, bool dsp)
{
	unsigned int i;
	int ret;

	if (s->running)
		return 0;
	ret = zsp_service_start(&s->voice, &s->spi->dev);
	if (ret) {
		s->fault = true;
		return ret;
	}
	ret = identify_for_init(s);
	if (ret)
		goto failed;
	if (s->identity[1] != 0xb3) {
		dev_err(&s->spi->dev, "RCN/PCN %02x %02x, expected PCN b3\n",
			s->identity[0], s->identity[1]);
		ret = -ENODEV;
		goto failed;
	}
	s->identified = true;
	ret = vp_result(s, VpMakeDeviceObject(VP_DEV_880_SERIES, &s->id, &s->ctx, &s->devobj));
	for (i = 0; !ret && i < 2; i++)
		ret = vp_result(s, VpMakeLineObject(VP_TERM_FXS_GENERIC, i,
				&s->lines[i], &s->lineobj[i], &s->ctx));
	if (!ret)
		ret = vp_result(s, VpInitDevice(&s->ctx, dev_profile, ac_600r_fxs,
				dc_25ma_cc, ring_25hz_sine, NULL, NULL));
	if (!ret)
		ret = wait_event_id(s, VP_DEV_EVID_DEV_INIT_CMP, NULL);
	for (i = 0; !ret && i < 2; i++) {
		VpLineCtxType *line = &s->lines[i];
		VpOptionEventMaskType mask = {};
		ret = vp_result(s, VpSetOption(line, NULL, VP_OPTION_ID_EVENT_MASK, &mask));
		if (!ret)
			ret = vp_result(s, VpSetLineState(line, VP_LINE_STANDBY));
		if (!ret)
			ret = vp_result(s, VpCalLine(line));
		if (!ret)
			ret = wait_event_id(s, VP_EVID_CAL_CMP, line);
		if (!ret)
			ret = vp_result(s, VpSetLineState(line, VP_LINE_DISCONNECT));
	}
	if (!ret)
		ret = configure_pcm(s, dsp);
	if (ret)
		goto failed;
	s->fault = false;
	s->running = true;
	wake_up(&s->wait);
	return 0;
failed:
	ret = stop(s) ?: ret;
	s->fault = true;
	return ret;
}

static int poll_lines(void *arg)
{
	struct le88266 *s = arg;
	int err;

	while (!kthread_should_stop()) {
		wait_event_interruptible(s->wait, READ_ONCE(s->running) || kthread_should_stop());
		if (kthread_should_stop())
			break;
		mutex_lock(&s->lock);
		err = 0;
		if (s->running) {
			err = zsp_service_poll(&s->voice);
			err = err ?: tick(s, 0, NULL);
			if (!err && s->intercom)
				err = intercom_hooks(s);
			err = err ?: advance_calls(s);
			if (!err && s->intercom)
				err = intercom_audio(s);
		}
		if (err) {
			dev_err(&s->spi->dev, "line service stopped: %d\n", err);
			stop(s);
			s->fault = true;
		}
		mutex_unlock(&s->lock);
		usleep_range(5000, 5500);
	}
	return 0;
}

static ssize_t service_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct le88266 *s = dev_get_drvdata(dev);
	ssize_t n;

	mutex_lock(&s->lock);
	n = sysfs_emit(buf, "%s\n", s->fault ? "fault" : s->intercom ? "intercom" : s->media ? "media" : s->running ? "on" : "off");
	mutex_unlock(&s->lock);
	return n;
}

static ssize_t service_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct le88266 *s = dev_get_drvdata(dev);
	bool bridge = sysfs_streq(buf, "intercom"), on = true;
	bool media = sysfs_streq(buf, "media");
	unsigned int ch;
	int err = (bridge || media) ? 0 : kstrtobool(buf, &on);

	if (err)
		return err;
	mutex_lock(&s->lock);
	if (s->streams && (!on || bridge || media)) {
		err = -EBUSY;
		goto out;
	}
	if ((s->intercom && bridge) || (s->media && media))
		goto out;
	if (!on || (s->running && (s->intercom != bridge || s->media != media)))
		err = stop(s);
	if (!err && on)
		err = start(s, bridge || media);
	if (err || (!bridge && !media))
		goto out;
	err = zsp_service_channels(&s->voice);
	if (!err && media) {
		s->media = true;
		goto out;
	}
	for (ch = 0; !err && ch < ARRAY_SIZE(s->lines); ch++)
		err = intercom_line(s, ch, BRIDGED);
	if (!err) {
		s->hooks = 0;
		s->intercom = true;
		err = intercom_hooks(s);
	}
	if (err) {
		stop(s);
		s->fault = true;
	}
out:
	mutex_unlock(&s->lock);
	return err ?: count;
}
static DEVICE_ATTR_RW(service);

/* With the service off, enable the digital clocks only for the MPI read. */
static ssize_t chip_id_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct le88266 *s = dev_get_drvdata(dev);
	int ret = 0, cleanup;

	mutex_lock(&s->lock);
	if (!s->running) {
		if (s->fault) {
			ret = -EIO;
			goto out;
		}
		ret = zsp_service_start(&s->voice, dev);
		if (ret) {
			s->fault = !!s->voice.hw.shared.io;
			goto out;
		}
		ret = read_identity(s);
		cleanup = zsp_service_stop(&s->voice);
		if (cleanup) {
			dev_err(dev, "DSP shutdown after MPI identity read failed: %d\n", cleanup);
			s->fault = true;
			ret = ret ?: cleanup;
		}
	}
	if (!ret)
		ret = sysfs_emit(buf, "%02x %02x\n", s->identity[0], s->identity[1]);
out:
	mutex_unlock(&s->lock);
	return ret;
}
static DEVICE_ATTR_ADMIN_RO(chip_id);

static const struct { const char *name; VpLineStateType state; } modes[] = {
	{ "disconnect", VP_LINE_DISCONNECT }, { "standby", VP_LINE_STANDBY },
	{ "talk", VP_LINE_TALK }, { "ring", VP_LINE_RINGING },
};

static ssize_t line_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct le88266 *s = dev_get_drvdata(dev);
	unsigned int ch = tel_channel(s, attr), i;
	VpLineStateType state;
	bool hook;
	ssize_t ret;

	mutex_lock(&s->lock);
	ret = s->running ? vp_result(s, VpGetLineState(&s->lines[ch], &state)) : -EHOSTDOWN;
	if (!ret)
		ret = vp_result(s, VpGetLineStatus(&s->lines[ch], VP_INPUT_HOOK, &hook));
	if (!ret) {
		for (i = 0; i < ARRAY_SIZE(modes) && modes[i].state != state; i++)
			;
		ret = sysfs_emit(buf, "%s %s\n", s->call[ch] ? calls[s->call[ch]] : i < ARRAY_SIZE(modes) ? modes[i].name : "other",
				 hook ? "off-hook" : "on-hook");
	}
	mutex_unlock(&s->lock);
	return ret;
}

static ssize_t line_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct le88266 *s = dev_get_drvdata(dev);
	unsigned int ch = tel_channel(s, attr), i;
	enum call_mode mode = DIRECT;
	bool hook;
	int ret;

	for (i = PHONE; i <= ALERT; i++) {
		if (sysfs_streq(buf, calls[i])) {
			mode = i;
			break;
		}
	}
	for (i = 0; i < ARRAY_SIZE(modes); i++)
		if (sysfs_streq(buf, modes[i].name))
			break;
	if (mode == DIRECT && i == ARRAY_SIZE(modes))
		return -EINVAL;
	mutex_lock(&s->lock);
	if (!s->running) {
		ret = -EHOSTDOWN;
		goto out;
	}
	if (s->intercom) {
		ret = -EBUSY;
		goto out;
	}
	ret = vp_result(s, VpGetLineStatus(&s->lines[ch], VP_INPUT_HOOK, &hook));
	if (ret)
		goto fail;
	if ((mode == ALERT && hook) || ((mode == BUSY || mode == RINGBACK) && !hook)) {
		ret = -EBUSY;
		goto out;
	}
	ret = vp_result(s, VpSetLineTone(&s->lines[ch], NULL, NULL, NULL));
	if (ret)
		goto fail;
	s->call[ch] = mode;
	switch (mode) {
	case DIRECT:
		ret = vp_result(s, VpSetLineState(&s->lines[ch], modes[i].state));
		break;
	case PHONE:
		ret = phone_hook(s, ch, hook);
		break;
	case ALERT:
		s->alert_end[ch] = jiffies + msecs_to_jiffies(60000);
		ret = set_phase(s, ch, true);
		break;
	case BUSY:
	case RINGBACK:
		ret = vp_result(s, VpSetLineState(&s->lines[ch], VP_LINE_TALK));
		ret = ret ?: set_phase(s, ch, true);
		break;
	default:
		ret = -EINVAL;
	}
fail:
	if (ret) {
		stop(s);
		s->fault = true;
	}
out:
	mutex_unlock(&s->lock);
	return ret ?: count;
}
static DEVICE_ATTR(tel1, 0644, line_show, line_store);
static DEVICE_ATTR(tel2, 0644, line_show, line_store);
static ssize_t media_stats_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct le88266 *s = dev_get_drvdata(dev);
	unsigned int ch = attr->attr.name[3] - '1';
	ssize_t len;

	mutex_lock(&s->lock);
	len = sysfs_emit(buf, "mic=%u playback_queued=%u playback_busy=%u\n",
			 s->voice.audio[ch].mic_frames, s->voice.audio[ch].playback_queued,
			 s->voice.audio[ch].playback_busy);
	mutex_unlock(&s->lock);
	return len;
}
static DEVICE_ATTR(tel1_media, 0444, media_stats_show, NULL);
static DEVICE_ATTR(tel2_media, 0444, media_stats_show, NULL);
/* Read the active PCM configuration over MPI and compare the codec's
 * TX monitor with complete frames already consumed by the normal DSP path.
 * CF also consumes dial-pulse samples. Leave that FIFO to VoicePath when
 * either line uses pulse decoding. Reading this file never starts a service.
 */
static ssize_t pcm_status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct le88266 *s = dev_get_drvdata(dev);
	static const u8 commands[] = { 0x57, 0x71, 0x41, 0x43, 0x61, 0xa7 };
	u8 mode, clock, enabled, regs[ARRAY_SIZE(commands)], samples[14], ec;
	unsigned int ch, i, count;
	ssize_t n = 0;
	int ret = -EHOSTDOWN;

	mutex_lock(&s->lock);
	if (!(s->intercom || s->media))
		goto out;
	ret = mpi_transfer(s, NULL, 0x4b, 1, &enabled);
	ret = ret ?: mpi_transfer(s, NULL, 0x5f, 1, &mode);
	ret = ret ?: mpi_transfer(s, NULL, 0x45, 1, &clock);
	if (ret)
		goto out;
	n = sysfs_emit(buf, "clock_slot=%02x device_mode=%02x ec=%02x\n", clock, mode, enabled);
	for (ch = 0; ch < ARRAY_SIZE(s->lines); ch++) {
		unsigned int dsp = dsp_channel(s, ch);

		/* Selecting the monitor channel also writes the global WB bit. */
		ec = s->lineobj[ch].ecVal;
		for (i = 0; i < ARRAY_SIZE(commands); i++) {
			ret = mpi_transfer(s, &ec, commands[i], 1, &regs[i]);
			if (ret)
				goto out;
		}
		n += sysfs_emit_at(buf, n,
			"%s ch=%u dsp=%u state=%02x opcond=%02x txslot=%u rxslot=%u opfunc=%02x adc=%02x dsp_frames=%u nonidle_samples=%u varying_frames=%u\n",
			tel_name(s, ch), ch, dsp, regs[0], regs[1], regs[2], regs[3], regs[4], regs[5],
			s->voice.audio[dsp].mic_frames, s->voice.audio[dsp].mic_nonidle,
			s->voice.audio[dsp].mic_varying);
		if (!(regs[0] & BIT(5)))
			continue;
		if (mode & BIT(0)) {
			if (ch != !!(mode & BIT(1)) ||
			    s->lineobj[0].pulseMode != VP_OPTION_PULSE_DECODE_OFF ||
			    s->lineobj[1].pulseMode != VP_OPTION_PULSE_DECODE_OFF)
				continue;
			ret = mpi_transfer(s, &ec, 0xcf, sizeof(samples), samples);
			if (ret)
				goto out;
			count = min_t(unsigned int, (samples[0] >> 4) & 7, 6);
			n += sysfs_emit_at(buf, n, "%s slic_tx_words=%*phN\n",
					   tel_name(s, ch), (int)(2 * count), samples + 2);
		} else {
			/* CBS selects the device monitor. Match EC as well. */
			if (ch != !!(mode & BIT(1)))
				continue;
			ret = mpi_transfer(s, &ec, 0xcd, 2, samples);
			if (ret)
				goto out;
			n += sysfs_emit_at(buf, n, "%s slic_tx_words=%2phN\n",
					   tel_name(s, ch), samples);
		}
	}
out:
	mutex_unlock(&s->lock);
	return ret ?: n;
}
static DEVICE_ATTR_ADMIN_RO(pcm_status);

static struct attribute *fxs_attrs[] = {
	&dev_attr_service.attr, &dev_attr_chip_id.attr, &dev_attr_tel1.attr, &dev_attr_tel2.attr,
	&dev_attr_tel1_media.attr, &dev_attr_tel2_media.attr, &dev_attr_pcm_status.attr, NULL
};
static ssize_t audio_transfer(struct kobject *kobj, struct bin_attribute *attr,
			      char *buf, loff_t off, size_t size, bool playback)
{
	struct le88266 *s = dev_get_drvdata(kobj_to_dev(kobj));
	unsigned int ch = attr->attr.name[3] - '1';
	int ret;

	if (off || size != 80)
		return -EINVAL;
	mutex_lock(&s->lock);
	ret = s->media ? zsp_service_audio(&s->voice, ch, playback, buf) : -EHOSTDOWN;
	mutex_unlock(&s->lock);
	return ret ?: size;
}

static ssize_t audio_read(struct file *file, struct kobject *kobj, struct bin_attribute *attr,
			  char *buf, loff_t off, size_t size)
{
	return audio_transfer(kobj, attr, buf, off, size, false);
}

static ssize_t audio_write(struct file *file, struct kobject *kobj, struct bin_attribute *attr,
			   char *buf, loff_t off, size_t size)
{
	return audio_transfer(kobj, attr, buf, off, size, true);
}

static struct bin_attribute tel1_audio = __BIN_ATTR(tel1_audio, 0600, audio_read, audio_write, 80);
static struct bin_attribute tel2_audio = __BIN_ATTR(tel2_audio, 0600, audio_read, audio_write, 80);
static struct bin_attribute *fxs_bin_attrs[] = { &tel1_audio, &tel2_audio, NULL };
static const struct attribute_group fxs_group = { .attrs = fxs_attrs, .bin_attrs = fxs_bin_attrs };
static const struct attribute_group *fxs_groups[] = { &fxs_group, NULL };

static int pcm_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params,
		      struct snd_soc_dai *dai)
{
	struct le88266 *s = dev_get_drvdata(dai->dev);
	VpOptionCodecType law;
	unsigned int stream = BIT(substream->stream);
	int ret = 0;

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_A_LAW:
		law = VP_OPTION_ALAW;
		break;
	case SNDRV_PCM_FORMAT_MU_LAW:
		law = VP_OPTION_MLAW;
		break;
	default:
		return -EINVAL;
	}
	mutex_lock(&s->lock);
	if (!s->running) {
		ret = -EHOSTDOWN;
	} else if (s->intercom || s->media) {
		ret = -EBUSY;
	} else if ((s->streams & ~stream) && s->law != law) {
		ret = -EBUSY;
	} else {
		ret = vp_result(s, VpSetOption(NULL, &s->ctx, VP_OPTION_ID_CODEC, &law));
		if (!ret) {
			s->law = law;
			s->streams |= stream;
		}
	}
	mutex_unlock(&s->lock);
	return ret;
}

static int pcm_free(struct snd_pcm_substream *substream, struct snd_soc_dai *dai)
{
	struct le88266 *s = dev_get_drvdata(dai->dev);

	mutex_lock(&s->lock);
	s->streams &= ~BIT(substream->stream);
	mutex_unlock(&s->lock);
	return 0;
}

static const struct snd_soc_dai_ops fxs_dai_ops = {
	.hw_params = pcm_params, .hw_free = pcm_free,
};

#define FXS_STREAM(label) { .stream_name = label, .channels_min = 2, .channels_max = 2, \
	.rates = SNDRV_PCM_RATE_8000, .formats = SNDRV_PCM_FMTBIT_A_LAW | SNDRV_PCM_FMTBIT_MU_LAW }

static struct snd_soc_dai_driver fxs_dai = {
	.name = "le88266-pcm", .playback = FXS_STREAM("FXS Playback"),
	.capture = FXS_STREAM("FXS Capture"), .ops = &fxs_dai_ops,
};

static const struct snd_soc_component_driver fxs_component = {
	.name = "le88266",
};

static int fxs_probe(struct spi_device *spi)
{
	struct device_node *node;
	struct resource res;
	struct le88266 *s;
	int err;

	s = devm_kzalloc(&spi->dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->ports_swapped = of_property_read_bool(spi->dev.of_node, "huawei,tel-ports-swapped");
	node = of_parse_phandle(spi->dev.of_node, "hisilicon,highway", 0);
	if (!node)
		return -EINVAL;
	s->voice.hw.crg = syscon_regmap_lookup_by_phandle(node, "hisilicon,crg");
	err = of_address_to_resource(node, 0, &res);
	of_node_put(node);
	if (IS_ERR(s->voice.hw.crg))
		return PTR_ERR(s->voice.hw.crg);
	if (err || res.start != ZSP_HW_BASE || resource_size(&res) < ZSP_HW_BYTES)
		return -EINVAL;
	s->spi = spi;
	s->id.mpi = s;
	mutex_init(&s->lock);
	init_waitqueue_head(&s->wait);
	spi_set_drvdata(spi, s);
	err = devm_snd_soc_register_component(&spi->dev, &fxs_component, &fxs_dai, 1);
	if (err)
		return err;
	s->poller = kthread_run(poll_lines, s, "le88266/%s", dev_name(&spi->dev));
	return PTR_ERR_OR_ZERO(s->poller);
}

static void fxs_shutdown(struct spi_device *spi)
{
	struct le88266 *s = spi_get_drvdata(spi);

	mutex_lock(&s->lock);
	if (stop(s))
		dev_err(&spi->dev, "unable to confirm SLIC shutdown\n");
	mutex_unlock(&s->lock);
}

static void fxs_remove(struct spi_device *spi)
{
	struct le88266 *s = spi_get_drvdata(spi);

	kthread_stop(s->poller);
	fxs_shutdown(spi);
	if (s->voice.hw.shared.io) {
		/* Reset I/O failed. Release ownership without writing live DSP memory. */
		if (s->voice.pcm.armed)
			hg8245_pcm_finish(&s->voice.pcm, false);
		zsp_unmap_firmware(&s->voice.hw);
	}
}

static const struct of_device_id fxs_of[] = {
	{ .compatible = "zarlink,le88266" }, {}
};
MODULE_DEVICE_TABLE(of, fxs_of);
static const struct spi_device_id fxs_ids[] = { { "le88266", 0 }, {} };
MODULE_DEVICE_TABLE(spi, fxs_ids);
static struct spi_driver fxs_driver = {
	.driver = { .name = "le88266", .of_match_table = fxs_of, .dev_groups = fxs_groups },
	.id_table = fxs_ids, .probe = fxs_probe, .remove = fxs_remove, .shutdown = fxs_shutdown,
};
module_spi_driver(fxs_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Le88266 dual FXS line controller with VE880 VoicePath");
