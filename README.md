# lwIP-84+: lwIP-CE on the TI-84 Plus / TI-84 Plus Silver Edition

This directory ports [lwIP-CE](../README.md) to the original monochrome
**TI-84 Plus and TI-84 Plus Silver Edition** (Z80 at 15 MHz, 128 KiB RAM). The
TI-84 Plus CE build in `../src` is unchanged; the TI-84 Plus build shares the
same lwIP core, the `mem_buffer` allocator and the app-facing socket API
(`lwIP.h`: `lwip_start`, `lwip_network_up`, `lwip_service_events`,
`lwip_socket_*`).

Only the early hardware revisions with **128 KiB of RAM** (RAM pages 80h-87h)
are supported. The later revisions with 48 KiB of RAM are not.

Because TI-OS leaves too little memory for a network stack, lwIP-84+ is a small
**standalone operating system** that replaces TI-OS. TI-OS can be
reinstalled at any time from the boot code (see below).

## How the calculator connects

The calculator is a **USB CDC-NCM device**: plugged into a PC, it appears as a
USB network adapter (Linux `cdc_ncm`, Windows 10+ `UsbNcm`, macOS). The PC
provides the network, typically with DHCP and NAT ("connection sharing").
USB-host Ethernet adapters, as used by the CE version, are not supported on the
TI-84 Plus.

```
TI-84 Plus  --USB (CDC-NCM)-->  PC (DHCP + NAT)  -->  network
```

Linux with NetworkManager (the interface is named after the MAC address
`02:84:84:00:00:01`, for example `enx028484000001`):

```
nmcli connection add type ethernet ifname enx028484000001 con-name ti84 ipv4.method shared
```

On Windows, enable Internet Connection Sharing for the new adapter.

## Status

| Part | State |
|------|-------|
| Standalone OS (boot, LCD console, keypad, timer, power off) | works on hardware |
| USB peripheral driver (Mentor MUSBFDRC), CDC-NCM function | works on hardware (Linux `cdc_ncm`) |
| lwIP-CE core: ARP, IPv4, ICMP, UDP, TCP, DHCP, DNS | works on hardware |
| lwIP-CE socket API (`lwIP.c`) | HTTP GET over the Internet works on hardware |
| SNTP, plain WebSocket | work in emu84 |
| TLS 1.3 client (`port/tls84.c`), TLS/WSS sockets | works in emu84 (see [TLS](#tls)) |
| Text web browser (HTTP and HTTPS) | works in emu84 |
| SSH client (password login, shell) | works in emu84 against OpenSSH 9.6 |
| Power off with 2nd+ON | fixed (it woke up again at once); verified in emu84 |

Tested on a TI-84 Plus (hardware revision 2, boot code 1.00) connected to Linux
with NetworkManager in shared mode: DHCP lease, ping from the PC (about 19 ms),
HTTP GET of example.com (891 bytes in 1.4 s, debug build) and a TCP download
from the PC at about 6.2 KB/s.

## Building

Requirements: **SDCC 4.4.0** (4.5.0 miscompiles variable shifts such as
`8 >> i` in larger functions), Python 3, GCC for the emulator, and
[mktiupgrade](https://github.com/KnightOS/mktiupgrade) for the `.8xu` file.

```
make                      # TI-84 Plus: build-84p/lwip84.rom and build-84p/lwip84.8xu
make MODEL=84pse          # TI-84 Plus Silver Edition
make DEBUG=1              # debug build: USB/driver log on the LCD (or in emu84)
make emu                  # the test emulator, build-84p/emu84
```

The Makefile expects SDCC in `../../tools/sdcc-4.4.0/bin` and the other tools
in `../../tools/local/bin`; override with `SDCC_BIN=` and `TOOLS=`.
`tools/get-key.sh` downloads TI's public OS signing key `0A` (factored in 2009,
distributed by KnightOS) into `keys/`; the key is not part of this repository.

### Memory layout and code banking

```
0000-3FFF  flash page 0: reset/ISR, far-call thunks, constants, USB FIFO code
4000-7FFF  bank A: the flash page of the running code
8000-FFFF  RAM pages 1 and 0: data, heap (about 20 KiB), stack
```

The OS and the stack are about 200 KiB of Z80 code plus about 13 KiB in
page 0. `tools/banker.py` distributes the functions of all modules over
flash pages and rewrites the SDCC assembler output: calls inside a page stay
direct, while calls to another page and all function pointers go through a
4-byte thunk in page 0 (`rst 10h` followed by the page and the address).
The thunk switches bank A and keeps the return information on a separate
far stack, so the normal SDCC calling convention keeps working. Constants
are compiled into page 0 (`--constseg CONST`) so that pointers to them are
valid everywhere.

To keep page 0 and RAM free, data stays in flash where possible:

- constants whose name ends in `_bk` (the font, the SHA-256 constants, the
  keypad and character tables, the X25519 programs, the HTML tables) are
  moved by `banker.py` next to the function that uses them, and the
  functions of the module that use them are kept on the same page;
- the Z80 crypto code and its 512-byte table occupy one fixed page
  (`fixed@3B`, `7B` on the SE) so the table is page-aligned at 4000h.

RAM pages 82h-87h (96 KiB) are reached through `xram_read`/`xram_write`
(page 0, `os/far.c`) and hold the web page text and links, the terminal
scrollback and the console history. The browser and SSH buffers are
allocated only while the application runs (only one runs at a time); after
start-up about 20 KiB of the data window are free for the heap.

The boot code forbids executing code on flash pages 08h-29h (08h-69h on the
SE), so code is placed on pages 01-07 and 33-3B (73-7B on the SE), the same
pages TI-OS uses.

### Source layout

| Path | Contents |
|------|----------|
| `os/` | the OS: start-up, interrupts, far calls, LCD console, keypad (scanned in the timer interrupt), heap, USB driver (`usb.c`), CDC-NCM (`ncm.c`), menu and test console (`main.c`, `apps.c`, `demo.c`), text input (`input.c`), web browser (`browser.c`), SSH client and terminal (`ssh.c`, `term.c`) |
| `port/` | lwIP-CE platform layer: `sys_arch`, NCM netif, USB Ethernet driver entry points, SNTP clock, TLS 1.3 client (`tls84.c`), X25519 key pair (`kx84.c`) |
| `crypto/` | SHA-256/HMAC/HKDF, ChaCha20/Poly1305, X25519, random numbers |
| `include/` | `lwipopts.h`, `arch/cc.h` and small libc additions for SDCC |
| `emu/` | `emu84`, a headless emulator with a USB host model |
| `tools/` | `banker.py`, `mkrom.py`, profiling and helper scripts |

The shared lwIP-CE sources needed only small changes, all guarded by
`LWIP_PORT_TI84P` or portable: `core/init.c` (heap size), `lwip-imports.h`
(no usbdrvce table), `lwIP.c` (driver header), `app_config.h` (overridable
TLS floor), `altcp_ws.c` (no variable-length array).

## Applications

The OS starts in the network console. **APPS** opens the menu:

| Key | Application |
|-----|-------------|
| 1 | web browser |
| 2 | SSH client |
| 3 | network console (test console below) |

**2nd+ON** turns the calculator off everywhere; **ON** turns it on again.
The X25519 key pair needed by HTTPS and SSH is computed in the background
when the menu is opened (about 30 s, shown in the menu), so it is usually
ready when the first connection starts.

### Typing

| Key | Effect |
|-----|--------|
| ALPHA | cycles the mode: `1` digits, `A` upper case, `a` lower case (shown in the status line) |
| letters | the green ALPHA labels of the keypad (MATH = A ... 2 = Z); 0 is a space, `.` is `:`, `(-)` is `?`, `+` is `"`, 3 is `_` |
| digits mode | 0-9 `.` `,` `(` `)` `+` `-` `*` `/` `^`, STO is `=` |
| 2nd + key | one symbol: `,`→`;` `(`→`{` `)`→`}` `*`→`[` `-`→`]` `/`→`\|` `^`→`~` `+`→`'` `.`→`_` `0`→`` ` `` STO→`=` LOG→`<` LN→`>` x²→`\` x⁻¹→`$` MATH→`!` APPS→`@` PRGM→`#` SIN→`%` COS→`&` TAN→`*` `(-)`→`?` |
| DEL / CLEAR | delete a character / clear the line (CLEAR on an empty line cancels) |
| LEFT/RIGHT, UP/DOWN | move the cursor, start/end of the line |

### Web browser

HTTP/1.0 and HTTPS, rendered while the page arrives: tags are dropped,
block elements start new lines, text is word-wrapped to 24 columns, links
are numbered `[n]`, redirects and chunked bodies are followed. Non-ASCII
characters are shown as `?`. The page text (up to 2048 lines) and the
links are kept in extended RAM.

| Key | Action |
|-----|--------|
| Y= | enter a URL (`http://` is added when there is no scheme) |
| 0-9, ENTER | follow link `[n]` |
| UP/DOWN, LEFT/RIGHT | scroll by a line, by a screen |
| DEL | back |
| ENTER | reload |
| CLEAR | stop loading |
| MODE | back to the menu (the page is kept) |

HTTPS needs a key exchange of about 25-30 s before the first byte; a
21 KB page then loads in about 12 s over HTTPS and 7 s over HTTP (emu84).

### SSH client

Enter `host[:port]`, the user name and the password. Key exchange
`curve25519-sha256`, cipher `chacha20-poly1305@openssh.com`, password
authentication (keyboard-interactive is used with the same password when
the server only offers that). The terminal is a 24x9 VT100 subset; lines
scrolled off the top are kept in extended RAM.

| Key | Sends / does |
|-----|--------------|
| ENTER | CR |
| DEL | backspace (DEL, 7Fh) |
| CLEAR | Ctrl-C |
| VARS | Tab |
| MODE | Esc |
| XT,θ,n then a key | Ctrl + that key (for example XT,θ,n then D: Ctrl-D) |
| arrows | cursor keys |
| 2nd+UP / 2nd+DOWN | scroll back / forward |
| 2nd+MODE | disconnect and return to the menu |

The login takes about a minute in total (key pair, if not ready yet, then
the key exchange). The server's host key is **not** checked.

## Test console

The network console:

| Key | Action |
|-----|--------|
| Y= | status: USB registers, NTB counters, gateway, DNS, free heap |
| WINDOW | ping the gateway |
| ZOOM | DNS lookup of example.com |
| TRACE | HTTP GET http://example.com/ through `lwip_socket_*` |
| GRAPH | TCP echo test with gateway:8084 |
| VARS | download test from gateway:8085 (prints bytes/s) |
| MATH | HTTPS GET of https://gateway:4433/ (for example `openssl s_server -www`) |
| PRGM | X25519 self test (RFC 7748 vector) with timing |
| MODE / STAT | SNTP time sync / show the time |
| CLEAR | clear the screen |
| UP/DOWN | scroll back through the last 128 lines |
| APPS | application menu |
| 2nd, ON | power off (ON wakes the calculator) |

Applications are linked into the OS and use the same `lwIP.h` API as on the
CE; see `os/apps.c`, `os/browser.c` and `os/ssh.c`.

## Testing with the emulator

`emu84` runs the OS image headless and models the hardware the OS uses,
including the boot code's execution protection. With `-u` it attaches a
virtual USB host that enumerates the calculator, speaks CDC-NCM and bridges
the Ethernet frames to [passt](https://passt.top), which gives the emulated
calculator real network access without root rights:

```
tools/run-passt.sh /tmp/passt.sock &          # calculator 192.168.84.2, PC 192.168.84.1
build-84p/emu84 -d -u -n /tmp/passt.sock build-84p/lwip84.rom <<EOF
run 3000
key trace
run 6000
screen
EOF
```

Commands: `run MS`, `waitlog TEXT MS`, `key NAME [MS]`, `down`/`up NAME`,
`on`, `screen [FILE.pbm]`, `regs`, `dumpram FILE`, `profile on|FILE`, `quit`.
`EMU84_USB_VERBOSE=1` logs every USB transaction. Services on the PC's
`127.0.0.1` are reachable from the calculator as `192.168.84.1`.
`tools/typekeys.py` turns text into `key` commands for the keypad layout
above, for example to type a URL in the browser (APPS, 1, Y=):

```
python3 tools/typekeys.py --mode a https://192.168.84.1:4433/
```

emu84 models the ON interrupt as level-triggered like the hardware and
prints `low-power halt` / `woke from low power` when the OS turns the
calculator off and on.

## Installing on a calculator

From Linux, without TiLP (libticalcs is unpacked locally by
`tools/build-ti84link.sh`, no root needed):

```
sudo tools/host-setup.sh        # once: udev rules + NetworkManager profile "ti84-usb"
tools/flash-calc.sh --debug     # versions, backup + ROM dump, send the OS, wait for the network
```

`flash-calc.sh` stores the backup (all variables and apps, `calc-info.txt`
and a ROM dump) in `../../backup/DATE/` and stops if the boot code is 1.03 or
newer. `ti84link check FILE.8xu` parses an upgrade file like TiLP does.

To replace lwIP-84+ with a newer build, put the calculator into the boot
code's receive mode (remove a battery, hold DEL while putting it back), then
plug in the USB cable *after* "Waiting..." is shown and run
`ti84link sendos FILE.8xu --force` within about 50 seconds. The boot code
answers slowly; retry `sendos` every few seconds until it starts.
`tools/ti84link romdump` does not work yet (the dumper re-enumerates USB).

Manually:

1. Back up your files: TI-OS and the calculator's RAM are replaced.
2. Send `build-84p/lwip84.8xu` (or `build-84pse/` for the Silver Edition) as an
   OS upgrade with TiLP or TI Connect. Boot code 1.02 and older accept the
   signed upgrade directly; boot code 1.03 and newer need the same workaround
   as KnightOS (for example UOSRECV).
3. Connect the calculator to the PC with the mini-USB cable.

To go back to TI-OS: remove one battery, hold DEL while putting it back, and
send the TI-OS upgrade from Texas Instruments when the calculator shows
"Waiting... Please install operating system now".

## TLS

`port/tls84.c` is a minimal TLS 1.3 client behind lwIP-CE's
`altcp_tls_ce_*` entry points, so `LWIP_SOCKET_ALTCP_TLS` and
`LWIP_SOCKET_ALTCP_WSS` sockets work through `lwIP.h` as on the CE. Only
what is needed to talk to a server is implemented:

- one cipher suite, `TLS_CHACHA20_POLY1305_SHA256`, and one group, X25519
  (servers without ChaCha20, such as some IIS setups, refuse the
  handshake);
- **no certificate, CertificateVerify or Finished checks**: the server's
  encrypted handshake messages are only hashed into the transcript. The
  connection is encrypted but not authenticated, so an active man in the
  middle is not detected. TLS sockets therefore do not wait for SNTP;
- incoming records are decrypted as a stream and their Poly1305 tags are
  not checked, so 16 KiB records need no 16 KiB buffer (records up to 1 KiB
  are buffered to read their content type); outgoing records are sealed
  properly;
- no HelloRetryRequest, session resumption, early data or KeyUpdate; an
  empty client Certificate is sent if the server asks for one.

The SSH client (`os/ssh.c`) makes the same choices: the host key and its
signature are not checked and received MACs are not verified.

The X25519 key pair (`port/kx84.c`) is generated once in the background
and then reused for every TLS and SSH connection until the calculator is
reset, which halves the time of every handshake but gives up forward
secrecy between connections. The shared secret is computed one field
multiplication (about 7 ms) at a time from the main loop, so USB, TCP and
the keypad keep working during the 25-30 s it takes.

Speed (15 MHz Z80, emu84): X25519 about 25 s, ChaCha20 about 110k cycles
per 64-byte block (about 9 KB/s), SHA-256 in C. The field multiplication
(4-bit windows, multiples of b tabulated, 38*x table for the reduction)
and the ChaCha20 block function are Z80 assembly generated by
`tools/gen_fe_z80.py`; everything else is C (`crypto/`). `tests/crypto_test.c`
checks the C code against the RFC test vectors on the host:

```
gcc -O2 -I crypto -o /tmp/crypto_test tests/crypto_test.c crypto/sha256.c crypto/chacha.c crypto/x25519.c
/tmp/crypto_test
```

Random numbers (`crypto/rng.c`) come from a SHA-256 pool fed with the
timer, the Z80 refresh register and the timing of key presses.

## Third-party code

- `emu/third_party/z80.h`: Z80 emulator from [floooh/chips](https://github.com/floooh/chips), zlib license
- passt and mktiupgrade are used as external tools.
