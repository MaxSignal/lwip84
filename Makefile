# lwIP-84+ : lwIP on the TI-84 Plus / Plus SE (Z80) as a standalone OS.
#
#   make            release OS image    (build-84p/lwip84.rom, build-84p/lwip84.8xu)
#   make DEBUG=1    debug build with emulator console output (build-debug-84p/)
#   make MODEL=84pse  TI-84 Plus Silver Edition image
#   make emu        build the test emulator (build-84p/emu84)

# SDCC 4.4.0 is required: 4.5.0 miscompiles variable shifts such as
# (8 >> i) inside larger functions (shift count lost).
SDCC_BIN ?= $(abspath ../tools/sdcc-4.4.0/bin)
TOOLS    ?= $(abspath ../tools/local/bin)
export PATH := $(SDCC_BIN):$(TOOLS):$(PATH)
export TMPDIR ?= $(abspath ../tmp)

CC      = sdcc
AS      = sdasz80
LD      = sdldz80
PY      = python3 -I
SDCCLIB = $(abspath $(SDCC_BIN)/../share/sdcc/lib/z80)

# MODEL=84p (TI-84 Plus, 1 MiB flash) or MODEL=84pse (TI-84 Plus SE, 2 MiB).
# FIXED is the page of the Z80 field arithmetic and its tables (fe_z80.s).
# The boot code forbids code execution on flash pages 08h..29h (84+) or
# 08h..69h (84+SE), so banked code goes to pages 01-07 and to the pages
# TI-OS itself uses at the top of flash.  Page 3A (7A) is left out: the boot
# code keeps the OS header and signature at its start.
MODEL ?= 84p
ifeq ($(MODEL),84pse)
BANKS   = 1-7,73-79
FIXED   = 7B
PAGES   = 128
DEVICE  = TI-84+SE
else
BANKS   = 1-7,33-39
FIXED   = 3B
PAGES   = 64
DEVICE  = TI-84+
endif

ifeq ($(DEBUG),1)
B       = build-debug-$(MODEL)
DEFS    = -DTI84P_DEBUG
else
B       = build-$(MODEL)
DEFS    =
endif

CFLAGS  = -mz80 --std-sdcc11 --opt-code-size --constseg CONST $(DEFS) \
          -DFE_ASM '-D__attribute__(x)=' \
          -I include -I os -I lib/net -I lib/crypto -I lib/font \
          -I app/browser -I app/ssh -I app/console -I $(LWIP_SRC)/include

# modules that must stay in page 0 (interrupt path, runtime)
COMMON_C = os/isr.c os/far.c os/usb.c os/kbd.c
COMMON_S = os/crt0.s os/farcall.s os/usb_ll.s
# everything else is placed in flash pages 1.. by tools/banker.py:
# the OS (os/), the library (lib/) and the applications (app/)
BANKED_C = os/main.c os/input.c os/ui.c os/lcd.c os/fmt.c os/usb_ctl.c os/ncm.c os/heap.c \
           lib/net/sys_arch.c lib/net/time84.c lib/net/netif_ncm.c lib/net/net84.c \
           lib/net/sock84.c lib/net/tls84.c lib/net/kx84.c lib/font/font.c \
           lib/crypto/sha256.c lib/crypto/chacha.c lib/crypto/aes.c lib/crypto/x25519.c \
           lib/crypto/rng.c \
           app/browser/browser.c app/browser/html.c app/ssh/ssh.c app/ssh/term.c \
           app/console/apps.c app/console/demo.c \
           $(LWIP_C)

# lwIP (git submodule lwip/, STABLE-2_2_1_RELEASE); tcp_in.c is compiled
# with patches/tcp-persist-timer.patch applied
LWIP_SRC = lwip/src
LWIP_C = $(addprefix $(LWIP_SRC)/, \
           core/init.c core/def.c core/dns.c core/inet_chksum.c core/ip.c core/mem.c \
           core/memp.c core/netif.c core/pbuf.c core/raw.c core/tcp.c \
           core/tcp_out.c core/timeouts.c core/udp.c core/stats.c core/sys.c \
           core/ipv4/dhcp.c core/ipv4/etharp.c core/ipv4/icmp.c core/ipv4/ip4.c \
           core/ipv4/ip4_addr.c netif/ethernet.c core/altcp.c core/altcp_tcp.c \
           core/altcp_alloc.c apps/sntp/sntp.c)

COMMON_ASM = $(patsubst %.c,$(B)/asm/%.asm,$(COMMON_C)) $(patsubst %.s,$(B)/asm/%.asm,$(COMMON_S))
BANKED_ASM = $(patsubst $(LWIP_SRC)/%.c,$(B)/asm/lwip/%.asm,$(filter $(LWIP_SRC)/%,$(BANKED_C))) \
             $(patsubst %.c,$(B)/asm/%.asm,$(filter-out $(LWIP_SRC)/%,$(BANKED_C))) \
             $(B)/asm/lwip/core/tcp_in.asm

all: $(B)/lwip84.rom $(B)/lwip84.8xu

# no automatic dependencies with SDCC: every module depends on all port headers
HDRS = $(wildcard include/*.h include/*/*.h os/*.h lib/*/*.h app/*/*.h)

$(B)/asm/%.asm: %.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -S $< -o $@

$(B)/asm/lwip/%.asm: $(LWIP_SRC)/%.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -S $< -o $@

$(B)/patched/core/tcp_in.c: $(LWIP_SRC)/core/tcp_in.c patches/tcp-persist-timer.patch
	@mkdir -p $(dir $@)
	cp $< $@
	patch -s -p2 -d $(B)/patched < patches/tcp-persist-timer.patch

$(B)/asm/lwip/core/tcp_in.asm: $(B)/patched/core/tcp_in.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -S $< -o $@

$(B)/asm/%.asm: %.s
	@mkdir -p $(dir $@)
	cp $< $@

FIXED_ASM = $(B)/asm/lib/crypto/fe_z80.asm
$(FIXED_ASM): tools/gen_fe_z80.py tools/gen_sha256_z80.py tools/gen_poly1305_z80.py tools/gen_aes_z80.py
	@mkdir -p $(dir $@)
	$(PY) tools/gen_aes_z80.py $@.aes 0x4200 $@.sbox
	$(PY) tools/gen_fe_z80.py $@.fe $@.sbox
	$(PY) tools/gen_sha256_z80.py $@.sha
	$(PY) tools/gen_poly1305_z80.py $@.p13
	cat $@.fe $@.sha $@.p13 $@.aes > $@

$(B)/banked.stamp: $(COMMON_ASM) $(BANKED_ASM) $(FIXED_ASM) tools/banker.py tools/check_iy.py
	$(PY) tools/check_iy.py $(COMMON_ASM) $(BANKED_ASM)
	rm -rf $(B)/bk && mkdir -p $(B)/bk
	$(PY) tools/banker.py --outdir $(B)/bk --thunks $(B)/bk/thunks.s --map $(B)/banks.txt --ldargs $(B)/banks.lnk --banks $(BANKS) \
	    $(addprefix common:,$(COMMON_ASM)) $(addprefix banked:,$(BANKED_ASM)) fixed@$(FIXED):$(FIXED_ASM)
	cd $(B)/bk && for f in *.asm *.s; do $(AS) -plosgff $${f%.*}.rel $$f || exit 1; done
	touch $@

$(B)/lwip84.ihx: $(B)/banked.stamp
	cd $(B)/bk && $(LD) -n -m -w -i -b _CODE=0x0080 -b _DATA=0x8000 \
	    $$(cat ../banks.lnk) \
	    -k $(SDCCLIB) -l z80 ../lwip84.ihx crt0.rel $$(ls *.rel | grep -v '^crt0.rel$$')

# external font on the flash data pages (lib/font/font.h): the fonts listed in
# FONT (fonts/fonts.txt); FONT= for none
FONT ?= fonts/fonts.txt
ifneq ($(FONT),)
FONT_BIN = $(B)/font.bin
FONT_ARG = --data 08:$(FONT_BIN)
$(FONT_BIN): $(FONT) $(wildcard fonts/*/*) tools/mkfont84.py
	@mkdir -p $(B)
	$(PY) tools/mkfont84.py $< $@
endif

$(B)/lwip84.rom: $(B)/lwip84.ihx tools/mkrom.py $(FONT_BIN)
	$(PY) tools/mkrom.py --pages $(PAGES) --pagelist $(B)/pages.txt $(FONT_ARG) $< $@
	@grep -E "^_(CODE|CONST|HOME|GSINIT|GSFINAL|INITIALIZER|DATA|BSS|INITIALIZED) " $(B)/lwip84.map | sort -u

# OS upgrade for the calculator, signed with TI's public OS key 0A
# (tools/get-key.sh); send it with TiLP or TI Connect.
KEY ?= keys/0A.key
$(B)/lwip84.8xu: $(B)/lwip84.rom $(KEY)
	mktiupgrade -p -k $(KEY) -d $(DEVICE) -v 1.0 $< $@ $$(cat $(B)/pages.txt)

$(KEY):
	tools/get-key.sh

EMU_SRC = emu/emu84.c emu/usb.c
emu: $(B)/emu84
$(B)/emu84: $(EMU_SRC) emu/emu84.h
	@mkdir -p $(B)
	gcc -O2 -Wall -Wno-unused-function -o $@ $(EMU_SRC)

clean:
	rm -rf build-* build build-debug

.PHONY: all emu clean
