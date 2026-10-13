# lwIP-84+ : lwIP on the TI-84 Plus / Plus SE (Z80), as a standalone OS or
# as TI-OS applications.
#
#   make            release OS image    (build-84p/lwip84.rom, build-84p/lwip84.8xu)
#   make DEBUG=1    debug build with emulator console output (build-debug-84p/)
#   make MODEL=84pse  TI-84 Plus Silver Edition image
#   make emu        build the test emulator (build-84p/emu84)
#   make PLATFORM=tios APP=browser   TI-OS app (build-tios-browser/lwip84.8xk);
#                   APP=ssh for the SSH client
#
# kernel/ is shared; platform/os/ and platform/tios/ hold what differs
# (start-up, far calls, main).

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
# the conversion dictionaries: 20 pages in the middle of the SE's archive
DICT_PAGE   = 40
DICT_BUDGET = 327680
else
BANKS   = 1-7,33-39
FIXED   = 3B
PAGES   = 64
DEVICE  = TI-84+
DICT_PAGE   = 08
DICT_BUDGET = 131072
endif

ifeq ($(DEBUG),1)
B       = build-debug-$(MODEL)
DEFS    = -DTI84P_DEBUG
else
B       = build-$(MODEL)
DEFS    =
endif
# RAM48=1: for the calculators with 48 KiB of RAM (one extended RAM page)
ifeq ($(RAM48),1)
B      := $(B)-48k
DEFS   += -DTI84P_RAM48
endif
# PLATFORM=tios: a TI-OS Flash application for the TI-84 Plus with 128 KiB
# of RAM (platform/tios/): logical pages 1.., the common code and the
# constants copied to RAM at 8184h, the buffers of the 48 KiB build, no
# font.  APP=browser or APP=ssh: one application each, for RAM.
PLATFORM ?= os
ifeq ($(PLATFORM),tios)
APP    ?= browser
B      := $(subst -$(MODEL),-tios-$(APP),$(B))
DEFS   += -DTI84P_APP -DTI84P_RAM48
ifeq ($(APP),ssh)
DEFS   += -DTIOS_SSH
APP_NAME = lwIPssh
else
DEFS   += -DTIOS_BROWSER
APP_NAME = lwIPweb
endif
BANKS   = 2-F
FIXED   = 01
FONT    =
BANKER_OPT = --call-thunks
LINK_CODE = 0x8184
else
LINK_CODE = 0x0080
endif

CFLAGS  = -mz80 --std-sdcc11 --opt-code-size --constseg CONST $(DEFS) -DDICT_PAGE=0x$(DICT_PAGE) \
          -DFE_ASM '-D__attribute__(x)=' \
          -I include -I kernel -I lib/net -I lib/crypto -I lib/font -I lib/ime \
          -I app/browser -I app/ssh -I app/console -I $(LWIP_SRC)/include

# modules that must stay in page 0 (interrupt path, runtime; in the TI-OS
# app: in RAM)
COMMON_C = kernel/isr.c kernel/far.c kernel/usb.c kernel/kbd.c
COMMON_S = platform/$(PLATFORM)/crt0.s platform/$(PLATFORM)/farcall.s kernel/usb_ll.s
# everything else is placed in flash pages 1.. by tools/banker.py: the
# kernel (kernel/), the library (lib/) and the applications (app/)
BANKED_C = platform/$(PLATFORM)/main.c kernel/power.c \
           kernel/input.c kernel/ui.c kernel/lcd.c kernel/fmt.c kernel/usb_ctl.c kernel/ncm.c kernel/heap.c \
           lib/net/sys_arch.c lib/net/time84.c lib/net/netif_ncm.c lib/net/net84.c \
           lib/net/sock84.c lib/net/tls84.c lib/net/kx84.c lib/font/font.c lib/ime/ime.c lib/ime/dict.c \
           lib/crypto/sha256.c lib/crypto/chacha.c lib/crypto/aes.c lib/crypto/x25519.c \
           lib/crypto/rng.c \
           app/browser/browser.c app/browser/html.c app/ssh/ssh.c app/ssh/term.c \
           app/console/apps.c app/console/demo.c \
           $(LWIP_C)

# lwIP (git submodule lwip/, STABLE-2_2_1_RELEASE); tcp_in.c is compiled
# with patches/tcp-persist-timer.patch and tcp-syn-nrtx.patch applied
LWIP_SRC = lwip/src
LWIP_C = $(addprefix $(LWIP_SRC)/, \
           core/init.c core/def.c core/dns.c core/inet_chksum.c core/ip.c core/mem.c \
           core/memp.c core/netif.c core/pbuf.c core/raw.c core/tcp.c \
           core/tcp_out.c core/timeouts.c core/udp.c core/stats.c core/sys.c \
           core/ipv4/dhcp.c core/ipv4/etharp.c core/ipv4/icmp.c core/ipv4/ip4.c \
           core/ipv4/ip4_addr.c netif/ethernet.c core/altcp.c core/altcp_tcp.c \
           core/altcp_alloc.c apps/sntp/sntp.c)

ifeq ($(PLATFORM),tios)
# a TI-OS app: one application, no console
BANKED_C := $(filter-out app/console/%,$(BANKED_C))
ifeq ($(APP),ssh)
BANKED_C := $(filter-out app/browser/%,$(BANKED_C))
else
BANKED_C := $(filter-out app/ssh/%,$(BANKED_C))
endif
endif

COMMON_ASM = $(patsubst %.c,$(B)/asm/%.asm,$(COMMON_C)) $(patsubst %.s,$(B)/asm/%.asm,$(COMMON_S))
BANKED_ASM = $(patsubst $(LWIP_SRC)/%.c,$(B)/asm/lwip/%.asm,$(filter $(LWIP_SRC)/%,$(BANKED_C))) \
             $(patsubst %.c,$(B)/asm/%.asm,$(filter-out $(LWIP_SRC)/%,$(BANKED_C))) \
             $(B)/asm/lwip/core/tcp_in.asm

ifeq ($(PLATFORM),tios)
all: $(B)/lwip84.8xk
else
all: $(B)/lwip84.rom $(B)/lwip84.8xu
endif

# no automatic dependencies with SDCC: every module depends on all port headers
HDRS = $(wildcard include/*.h include/*/*.h kernel/*.h lib/*/*.h app/*/*.h)

$(B)/asm/%.asm: %.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -S $< -o $@

$(B)/asm/lwip/%.asm: $(LWIP_SRC)/%.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -S $< -o $@

$(B)/patched/core/tcp_in.c: $(LWIP_SRC)/core/tcp_in.c patches/tcp-persist-timer.patch patches/tcp-syn-nrtx.patch
	@mkdir -p $(dir $@)
	cp $< $@
	patch -s -p2 -d $(B)/patched < patches/tcp-persist-timer.patch
	patch -s -p2 -d $(B)/patched < patches/tcp-syn-nrtx.patch

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
	$(PY) tools/banker.py $(BANKER_OPT) --outdir $(B)/bk --thunks $(B)/bk/thunks.s --map $(B)/banks.txt --ldargs $(B)/banks.lnk --banks $(BANKS) \
	    $(addprefix common:,$(COMMON_ASM)) $(addprefix banked:,$(BANKED_ASM)) fixed@$(FIXED):$(FIXED_ASM)
	cd $(B)/bk && for f in *.asm *.s; do $(AS) -plosgff $${f%.*}.rel $$f || exit 1; done
ifeq ($(PLATFORM),tios)
	$(AS) -plosgff $(B)/bk/launch.rel platform/tios/launch.s
endif
	touch $@

ifeq ($(PLATFORM),tios)
# the data follows the RAM image: link once to find where it ends
$(B)/lwip84.ihx: $(B)/banked.stamp
	cd $(B)/bk && $(LD) -n -m -w -i -b _CODE=$(LINK_CODE) -b _DATA=0xC000 \
	    $$(cat ../banks.lnk) \
	    -k $(SDCCLIB) -l z80 ../lwip84.ihx crt0.rel $$(ls *.rel | grep -v '^crt0.rel$$')
	cd $(B)/bk && end=$$(python3 -c "import re;m=open('../lwip84.map').read();f=lambda n:int(re.search(r'\b([0-9A-F]{8})\s+'+n+r'\b',m).group(1),16);print(hex(f('s__GSFINAL')+f('l__GSFINAL')))") && \
	    $(LD) -n -m -w -i -b _CODE=$(LINK_CODE) -b _DATA=$$end \
	    $$(cat ../banks.lnk) \
	    -k $(SDCCLIB) -l z80 ../lwip84.ihx crt0.rel $$(ls *.rel | grep -v '^crt0.rel$$')

$(B)/lwip84.bin: $(B)/lwip84.ihx tools/mkapp84.py
	$(PY) tools/mkapp84.py --name $(APP_NAME) $< $(B)/lwip84.map $@
	@grep -E "^_(CODE|CONST|HOME|GSINIT|GSFINAL|INITIALIZER|DATA|BSS|INITIALIZED|HEAP) " $(B)/lwip84.map | sort -u

# signed with the "shareware" app key 0104 (built into rabbitsign)
$(B)/lwip84.8xk: $(B)/lwip84.bin
	rabbitsign -b -g -o $@ $<
else
$(B)/lwip84.ihx: $(B)/banked.stamp
	cd $(B)/bk && $(LD) -n -m -w -i -b _CODE=$(LINK_CODE) -b _DATA=0x8000 \
	    $$(cat ../banks.lnk) \
	    -k $(SDCCLIB) -l z80 ../lwip84.ihx crt0.rel $$(ls *.rel | grep -v '^crt0.rel$$')
endif

# external font on the flash data pages (lib/font/font.h): the fonts listed in
# FONT (fonts/fonts.txt); FONT= for none
FONT ?= fonts/fonts.txt
ifneq ($(FONT),)
FONT_BIN = $(B)/font.bin
FONT_ARG = --data 20:$(FONT_BIN)
$(FONT_BIN): $(FONT) $(wildcard fonts/*/*) tools/mkfont84.py
	@mkdir -p $(B)
	$(PY) tools/mkfont84.py $< $@
endif

# the conversion dictionaries of the input methods (lib/ime/dict.h), from
# dict/*.tsv (tools/mkdict84.py extract makes them from Mozc and Unihan)
DICT_BIN = $(B)/dict.bin
$(DICT_BIN): dict/ja.tsv dict/zh.tsv tools/mkdict84.py
	@mkdir -p $(B)
	$(PY) tools/mkdict84.py build dict $@ $(DICT_BUDGET)

# the browser's Shift_JIS table (app/browser/html.h: HTML_JIS_PAGE)
JIS_BIN = $(B)/jis.bin
$(JIS_BIN): tools/mkjis84.py
	@mkdir -p $(B)
	$(PY) tools/mkjis84.py $@

$(B)/lwip84.rom: $(B)/lwip84.ihx tools/mkrom.py $(FONT_BIN) $(JIS_BIN) $(DICT_BIN)
	$(PY) tools/mkrom.py --pages $(PAGES) --pagelist $(B)/pages.txt $(FONT_ARG) \
	    --data 2A:$(JIS_BIN) --data $(DICT_PAGE):$(DICT_BIN) $< $@
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
