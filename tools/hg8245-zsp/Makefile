obj-m := hg8245_zsp_stage.o
hg8245_zsp_stage-y := zsp-stage-module.o zsp-loader.o zsp-hif.o zsp-hardware.o

ifneq ($(wildcard $(src)/hg8245_slicprobe.c),)
hg8245_zsp_stage-y += hg8245_slicprobe.o zsp-pcm.o
CFLAGS_zsp-stage-module.o += -DHG8245_ZSP_CODEC_LAB
CFLAGS_hg8245_slicprobe.o += -DHG8245_SLICPROBE_EMBEDDED
endif
