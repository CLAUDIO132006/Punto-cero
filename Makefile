EE_BIN = punto-cero.elf
EE_OBJS = src/main.o src/dev9_irx.o src/netman_irx.o src/smap_irx.o src/assets_data.o src/models_data.o src/audsrv_irx.o src/sio2man_irx.o src/padman_irx.o src/libsd_irx.o
EE_CFLAGS += -DPC_ONLINE -DPS2_ONLINE
EE_INCS += -I$(GSKIT)/include -I$(PS2SDK)/ports/include
EE_LDFLAGS += -L$(GSKIT)/lib
EE_LIBS = -lgskit -ldmakit -lnetman -lps2ip -lpad -ldebug -laudsrv -lpatches -lm -lc -lkernel

all: src/dev9_irx.c src/netman_irx.c src/smap_irx.c src/models_data.c src/sio2man_irx.c src/padman_irx.c src/libsd_irx.c src/assets_data.c $(EE_BIN)

# Los sonidos (assets/*.wav) se incrustan en el ELF. Para regenerarlos: make assets
src/models_data.c: $(wildcard assets/models/*.obj) $(wildcard assets/models/*.mtl)
	python3 tools/embed_models.py

src/assets_data.c: $(wildcard assets/sounds/*.wav) $(wildcard assets/music/*.wav)
	python3 tools/embed_assets.py assets src/assets_data.c

assets:
	gcc -O2 -w -Isrc -o export_assets tools/export_assets.c src/sound.c -lm && ./export_assets assets && rm -f export_assets
	python3 tools/embed_assets.py assets src/assets_data.c

src/dev9_irx.c: $(PS2SDK)/iop/irx/ps2dev9.irx
	$(PS2SDK)/bin/bin2c $< $@ DEV9_irx

src/netman_irx.c: $(PS2SDK)/iop/irx/netman.irx
	$(PS2SDK)/bin/bin2c $< $@ NETMAN_irx

src/smap_irx.c: $(PS2SDK)/iop/irx/smap.irx
	$(PS2SDK)/bin/bin2c $< $@ SMAP_irx

src/%_irx.c: $(PS2SDK)/iop/irx/%.irx
	$(PS2SDK)/bin/bin2c $< $@ $*_irx

clean:
	rm -f $(EE_BIN) $(EE_OBJS) src/assets_data.c src/dev9_irx.c src/netman_irx.c src/smap_irx.c

.PHONY: assets

run: $(EE_BIN)
	ps2client execee host:$(EE_BIN)

reset:
	ps2client reset

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
