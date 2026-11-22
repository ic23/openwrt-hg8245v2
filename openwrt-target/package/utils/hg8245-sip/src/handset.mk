include build.mak

hg8245-sip: handset.c
	$(APP_CC) $(APP_CFLAGS) -o $@ $< $(APP_LDFLAGS) -Wl,--gc-sections $(APP_LDLIBS)
