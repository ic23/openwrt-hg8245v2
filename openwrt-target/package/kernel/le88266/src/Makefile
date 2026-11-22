obj-m := le88266.o
ccflags-y += -I$(src) -I$(src)/port -I$(src)/includes -I$(src)/vp880_api
le88266-y := line.o zsp-pcm.o zsp-service.o zsp-hardware.o zsp-loader.o zsp-hif.o
vp_objs := $(patsubst $(src)/%.c,%.o,$(wildcard $(src)/common/*.c) $(wildcard $(src)/vp880_api/*.c))
le88266-y += $(vp_objs)
vp_flags := -include linux/kernel.h -include linux/string.h -Wno-implicit-fallthrough -Wno-switch-unreachable
$(foreach obj,$(vp_objs),$(eval CFLAGS_$(obj) += $(vp_flags)))
