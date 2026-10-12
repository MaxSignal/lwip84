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
| Text web browser (HTTP and HTTPS, forms, search) | works in emu84 |
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

Every push to the `ti84plus` branch runs `.github/workflows/ti84plus.yml`:
it builds the release and debug images for both models with the prebuilt
SDCC 4.4.0, runs the crypto tests below and publishes the `.8xu` and `.rom`
files as a release `ti84plus-build-N`.

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
| `os/` | the OS: start-up, interrupts, far calls, LCD console, keypad (scanned in the timer interrupt), heap, USB driver (`usb.c`), CDC-NCM (`ncm.c`), menu and test console (`main.c`, `apps.c`, `demo.c`), text input (`input.c`), web browser (`browser.c`, HTML renderer `html.c`), SSH client and terminal (`ssh.c`, `term.c`) |
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
In the default experimental speed mode the X25519 key pair needed by HTTPS
and SSH costs nothing (see [TLS](#tls)); with `KX_INSECURE_FAST=0` it is
computed in the background when the menu is opened (about 30 s).

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

HTTP/1.0 and HTTPS with a small HTML renderer of its own (`os/html.c`, no
CSS, no JavaScript). The page is laid out while it arrives:

- a streaming tokenizer that handles quoted attributes, comments, entities
  (named, numeric, accented letters) and raw text (`script`, `style`,
  `title`, `textarea`); UTF-8 is reduced to ASCII (accents are dropped,
  other characters become `?`);
- a stack of open elements that closes elements the way browsers do
  (`p`, `li`, `td`, `tr`, `option`, ...);
- invisible content is skipped: `script`, `style`, `svg`, `template`,
  elements with `hidden`, `aria-hidden="true"` or `display:none`, and a few
  classes (`noprint`, `navbox`, `mw-editsection`, `reference`, the menus of
  Wikipedia's Vector skin, ...);
- headings in capitals, lists with bullets or numbers and indentation,
  table cells side by side (`header: value` for `th`/`td` pairs), `pre`;
- links and form fields get numbers, drawn inverted in front of them;
- forms: text and password fields, text areas, check boxes, radio
  buttons, lists (`select`), submit buttons (`input` and `button`), hidden
  fields; sent with GET or POST (`application/x-www-form-urlencoded`).
  Controls outside a form and buttons that are not submit buttons need
  JavaScript and are left out.

The page text (up to 1600 lines), the items and their strings are kept in
extended RAM; the first screen appears while the rest is loading and
`<main>` is scrolled into view.

Words typed into the URL line (anything with a space, or without a dot)
are searched with [DuckDuckGo Lite](https://lite.duckduckgo.com/lite/),
which works without JavaScript; its result links are followed directly.
Google's search needs JavaScript since 2025 and shows only a notice.
DuckDuckGo may answer too many searches from one address with a picture
challenge, which cannot be solved here.

| Key | Action |
|-----|--------|
| Y= | URL or search words (`http://` is added when there is no scheme) |
| 0-9, ENTER | use item `n`: follow a link, edit a text field, toggle a check box, choose a radio button or a list entry, press a submit button |
| UP/DOWN, LEFT/RIGHT | scroll by a line, by a screen |
| GRAPH | top of the page / start of the main content |
| DEL | back |
| ENTER | reload |
| CLEAR | stop loading |
| MODE | back to the menu (the page is kept) |

While a text field is edited, ENTER stores it; in a form with only one
text field (search boxes) it also sends the form. A list opens a chooser
(UP/DOWN, ENTER, CLEAR).

Speed in emu84 (15 MHz): the renderer handles about 40 KB/s; with the
network stack a page arrives at about 7 KB/s over HTTP (a 337 KB Wikipedia
article in 49 s). HTTPS adds ChaCha20 at about 7.5 KB/s: a DuckDuckGo
search takes 10-15 s, the full Wikipedia article about 1.5 minutes, but its
beginning is on the screen after about 15 s. `tests/html_test.c` runs the
renderer on the host:

```
gcc -O2 -DHTML_HOST -I os -o ../../tmp/html_test tests/html_test.c os/html.c
../../tmp/html_test -i page.html
```

The TLS handshake with https://example.com/ (real certificate chain) takes
about 3 s of CPU time in emu84. The first byte of the page arrives 7.8 s
after ENTER, of which about 3 s is a TCP retransmission of the ClientHello
that passt causes in the emulator (it drops data sent before its own
outbound connection is up). It used to be about 40 s plus 30 s for the key
pair.

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

The shell appears about 3.3 s after connecting (emu84; it used to be 37 s
plus the key pair). The server's host key is **not** checked.

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
| 2nd, MATH | HTTPS GET of https://example.com/ |
| PRGM | crypto self test and timing (SHA-256, Poly1305, ChaCha20, X25519 RFC 7748 vector) |
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
- session resumption: the first NewSessionTicket of a connection is kept
  (one ticket, for the host that sent it, at most an hour) and offered with
  `psk_dhe_ke` on the next connection to the same host. A resumed handshake
  has no Certificate/CertificateVerify, so most of the SHA-256 work of a
  full handshake disappears (servers such as Cloudflare, Google, GitHub and
  OpenSSL send tickets; example.com does not);
- the TLS layer ACKs every incoming segment at once (no delayed ACK), so a
  server that writes the handshake as several small records with Nagle on
  is not held up;
- no HelloRetryRequest, early data or KeyUpdate; an empty client
  Certificate is sent if the server asks for one.

The SSH client (`os/ssh.c`) makes the same choices: the host key and its
signature are not checked and received MACs are not verified.

**Experimental speed mode (default, no security).** This port is an
experiment, so the handshakes are tuned for speed only:

- `KX_INSECURE_FAST` (`port/kx84.c`): the X25519 private key is 1. The public
  key is then the base point 9 and the shared secret equals the peer's public
  key (the peer computes s * 9, its own public key), so no scalar
  multiplication is needed at all. Servers see an ordinary X25519 exchange,
  but anybody who records the traffic can decrypt it.
- `RNG_INSECURE_FAST` (`crypto/rng.c`): client randoms, session ids and SSH
  cookies come from xorshift32 instead of SHA-256.
- HMAC-SHA-256 caches the ipad/opad states of the four most recent keys, so
  the TLS 1.3 key schedule needs about half the compressions.
- Received IP, TCP, UDP and ICMP checksums are not checked
  (`include/lwipopts.h`; USB packets carry a CRC of their own), which saves
  about 7% of the CPU time while a page is downloading.

Measured in emu84 (two artefacts of passt are left out: it drops data sent
before its outbound connection is up, which costs a 3 s TCP retransmission,
and it advertises a 256-byte window that triggers lwIP's 1.5 s persist
timer): TLS full handshake about 1.9 s after the ServerHello, resumed about
1.3 s; SSH key exchange about 0.9 s and the shell about 1.8 s after
connecting.

With `KX_INSECURE_FAST=0` the key pair is generated once in the background
and reused (about 30 s), and every shared secret costs a 26 s scalar
multiplication done one field operation at a time from the main loop.

Speed (15 MHz Z80, emu84, console PRGM): SHA-256 3.4 KB/s (264k cycles per
64-byte block), Poly1305 1 KB in 0.18 s, ChaCha20 8 KB/s, X25519 26 s
(only used by the self test now), extended RAM copies 580 KB/s. A TLS
handshake computes about 124 SHA-256 blocks (the transcript includes the
certificate chain) and that is now most of its time. The field
multiplication, ChaCha20, SHA-256 and Poly1305 are Z80 assembly generated by
`tools/gen_fe_z80.py`, `tools/gen_sha256_z80.py` and
`tools/gen_poly1305_z80.py` on the fixed crypto page; the rest is C
(`crypto/`). Code labels in these modules are functions to `banker.py`, so
tables must be addressed through equates (`name = .`), not labels.
`tests/asm_test.py` runs the SHA-256 and Poly1305 routines in a bare Z80
(`tests/z80run.c`) and compares them with Python:

```
gcc -O2 -o ../../tmp/z80run tests/z80run.c
python3 tests/asm_test.py sha256 20
python3 tests/asm_test.py poly1305 30
```

`tests/crypto_test.c` checks the C code against the RFC test vectors on the host:

```
gcc -O2 -I crypto -o /tmp/crypto_test tests/crypto_test.c crypto/sha256.c crypto/chacha.c crypto/x25519.c
/tmp/crypto_test
```

Random numbers (`crypto/rng.c`) come from a pool fed with the timer, the
Z80 refresh register and the timing of key presses; in the speed mode
xorshift32 expands it, otherwise SHA-256.

## Third-party code

- `emu/third_party/z80.h`: Z80 emulator from [floooh/chips](https://github.com/floooh/chips), zlib license
- passt and mktiupgrade are used as external tools.
