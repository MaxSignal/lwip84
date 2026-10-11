# lwIP-84+ : lwIP-CE on the TI-84 Plus / Plus SE (Z80) as a standalone OS.
#
#   make            release OS image   (build/lwip84.rom)
#   make DEBUG=1    debug build with emulator console output (build-debug/)
#   make emu        build the test emulator (build/emu84)

# SDCC 4.4.0 is required: 4.5.0 miscompiles variable shifts such as
# (8 >> i) inside larger functions (shift count lost).
SDCC_BIN ?= $(abspath ../../tools/sdcc-4.4.0/bin)
TOOLS    ?= $(abspath ../../tools/local/bin)
export PATH := $(SDCC_BIN):$(TOOLS):$(PATH)
export TMPDIR ?= $(abspath ../../tmp)

CC      = sdcc
AS      = sdasz80
LD      = sdldz80
PY      = python3 -I
SDCCLIB = $(abspath $(SDCC_BIN)/../share/sdcc/lib/z80)

ifeq ($(DEBUG),1)
B       = build-debug
DEFS    = -DDEBUG
else
B       = build
DEFS    =
endif

CFLAGS  = -mz80 --std-sdcc11 --opt-code-size --constseg CONST $(DEFS) \
          -I include -I os -I ../src/include

# modules that must stay in page 0 (interrupt path, runtime)
COMMON_C = os/isr.c
COMMON_S = os/crt0.s os/farcall.s
# everything else is placed in flash pages 1.. by tools/banker.py
BANKED_C = os/main.c os/lcd.c os/kbd.c os/fmt.c os/font3x5.c

COMMON_ASM = $(patsubst %.c,$(B)/asm/%.asm,$(COMMON_C)) $(patsubst %.s,$(B)/asm/%.asm,$(COMMON_S))
BANKED_ASM = $(patsubst %.c,$(B)/asm/%.asm,$(BANKED_C))

all: $(B)/lwip84.rom

$(B)/asm/%.asm: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -S $< -o $@

$(B)/asm/%.asm: %.s
	@mkdir -p $(dir $@)
	cp $< $@

$(B)/banked.stamp: $(COMMON_ASM) $(BANKED_ASM) tools/banker.py
	rm -rf $(B)/bk && mkdir -p $(B)/bk
	$(PY) tools/banker.py --outdir $(B)/bk --thunks $(B)/bk/thunks.s --map $(B)/banks.txt --ldargs $(B)/banks.lnk \
	    $(addprefix common:,$(COMMON_ASM)) $(addprefix banked:,$(BANKED_ASM))
	cd $(B)/bk && for f in *.asm *.s; do $(AS) -plosgff $${f%.*}.rel $$f || exit 1; done
	touch $@

$(B)/lwip84.ihx: $(B)/banked.stamp
	cd $(B)/bk && $(LD) -n -m -w -i -b _CODE=0x0080 -b _DATA=0x8000 \
	    $$(cat ../banks.lnk) \
	    -k $(SDCCLIB) -l z80 ../lwip84.ihx crt0.rel $$(ls *.rel | grep -v '^crt0.rel$$')

$(B)/lwip84.rom: $(B)/lwip84.ihx tools/mkrom.py
	$(PY) tools/mkrom.py $< $@
	@grep -E "^_(CODE|CONST|HOME|GSINIT|GSFINAL|INITIALIZER|DATA|BSS|INITIALIZED) " $(B)/lwip84.map | sort -u

EMU_SRC = emu/emu84.c emu/usb.c
emu: $(B)/emu84
$(B)/emu84: $(EMU_SRC) emu/emu84.h
	@mkdir -p $(B)
	gcc -O2 -Wall -Wno-unused-function -o $@ $(EMU_SRC)

clean:
	rm -rf build build-debug

.PHONY: all emu clean
