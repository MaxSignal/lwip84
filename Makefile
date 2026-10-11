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
          -DLWIP_PORT_TI84P=1 -DLWIP_TLS_FLOOR_BYTES=0u '-D__attribute__(x)=' \
          -I include -I os -I ../src/include -I ../src

# modules that must stay in page 0 (interrupt path, runtime)
COMMON_C = os/isr.c
COMMON_S = os/crt0.s os/farcall.s os/usb_ll.s
# everything else is placed in flash pages 1.. by tools/banker.py
BANKED_C = os/main.c os/lcd.c os/kbd.c os/fmt.c os/font3x5.c os/usb.c os/ncm.c os/heap.c \
           port/sys_arch.c port/sntp_time.c port/bytes.c port/imports.c port/netif_ncm.c \
           $(LWIP_C)

# lwIP-CE sources shared with the TI-84 Plus CE build
LWIP_SRC = ../src
LWIP_C = $(addprefix $(LWIP_SRC)/, \
           core/init.c core/def.c core/dns.c core/inet_chksum.c core/ip.c core/mem.c \
           core/memp.c core/netif.c core/pbuf.c core/raw.c core/tcp.c core/tcp_in.c \
           core/tcp_out.c core/timeouts.c core/udp.c core/stats.c core/sys.c \
           core/ipv4/dhcp.c core/ipv4/etharp.c core/ipv4/icmp.c core/ipv4/ip4.c \
           core/ipv4/ip4_addr.c netif/ethernet.c core/logging.c core/app_config.c \
           drivers/mem.c)

COMMON_ASM = $(patsubst %.c,$(B)/asm/%.asm,$(COMMON_C)) $(patsubst %.s,$(B)/asm/%.asm,$(COMMON_S))
BANKED_ASM = $(patsubst $(LWIP_SRC)/%.c,$(B)/asm/lwip/%.asm,$(filter $(LWIP_SRC)/%,$(BANKED_C))) \
             $(patsubst %.c,$(B)/asm/%.asm,$(filter-out $(LWIP_SRC)/%,$(BANKED_C)))

all: $(B)/lwip84.rom

$(B)/asm/%.asm: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -S $< -o $@

# shared lwIP-CE sources live outside this directory
$(B)/asm/lwip/%.asm: $(LWIP_SRC)/%.c
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
