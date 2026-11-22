/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HG8245_ZSP_SERVICE_H
#define HG8245_ZSP_SERVICE_H
#include "zsp-hardware.h"
#include "zsp-pcm.h"

struct zsp_service {
	struct zsp_hardware hw;
	struct hg8245_pcm pcm;
	u32 control, select, sram, config, irq, init;
	u32 timers[5];
	struct {
		u32 mic_frames, mic_nonidle, mic_varying, playback_queued, playback_busy;
	} audio[2];
	u32 media_clock;
	u64 media_epoch, next_play[2];
	bool changed;
	bool started_hw;
};

int zsp_service_start(struct zsp_service *voice, struct device *dev);
int zsp_service_stop(struct zsp_service *voice);
int zsp_service_channels(struct zsp_service *voice);
int zsp_service_flush_capture(struct zsp_service *voice);
int zsp_service_audio(struct zsp_service *voice, unsigned int ch, bool playback, u8 frame[80]);
int zsp_service_poll(struct zsp_service *voice);
#endif
