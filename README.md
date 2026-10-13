# lwIP-84+

A small operating system for the original monochrome **TI-84 Plus and
TI-84 Plus Silver Edition** (Z80 at 15 MHz, 128 KiB RAM) with a network
library — the [lwIP](https://savannah.nongnu.org/projects/lwip/) TCP/IP stack
on a USB network interface, sockets, TLS 1.3 and fonts for many scripts —
and applications built on it: a web browser, an SSH client and a network
console.

![Wikipedia's Zilog Z80 article on the calculator](docs/wikipedia-z80-text.png)
![The Japanese Wikipedia article on Z80](docs/wikipedia-z80-ja.png)

- Only the early hardware revisions with **128 KiB of RAM** are supported,
  not the later ones with 48 KiB.
- It replaces TI-OS, which leaves too little memory for a network stack.
  TI-OS can be reinstalled from the boot code at any time.
- lwIP 2.2.1 is a git submodule (`lwip/`), used unchanged except for
  `patches/`.

```
git clone --recursive https://github.com/MaxSignal/lwip84.git
```

| Path | Contents |
|------|----------|
| `os/` | the OS: start-up, interrupts, far calls, heap, LCD, keypad, USB and CDC-NCM drivers, text input, the application menu |
| `lib/net/` | the network library: lwIP glue, sockets, TLS 1.3 |
| `lib/crypto/` | SHA-256/HMAC/HKDF, ChaCha20/Poly1305, AES-128/GCM, X25519, random numbers |
| `lib/font/` | the external font (Japanese, Chinese, Hangul, Arabic, Devanagari, ...) |
| [`app/browser/`](app/browser/README.md) | web browser |
| [`app/ssh/`](app/ssh/README.md) | SSH client |
| [`app/console/`](app/console/README.md) | network console and socket examples |
| `emu/` | `emu84`, a headless emulator with a USB host |
| `tools/`, `tests/` | build tools, code generators, host tests |
| `fonts/`, `patches/`, `lwip/` | font sources, the lwIP fix, lwIP |

## Connecting

The calculator is a **USB CDC-NCM device**: on a PC it appears as a USB
network adapter (Linux `cdc_ncm`, Windows 10+ `UsbNcm`, macOS), and the PC
shares its network with DHCP and NAT.

```
TI-84 Plus  --USB (CDC-NCM)-->  PC (DHCP + NAT)  -->  network
```

Linux with NetworkManager (the interface is named after the MAC address
`02:84:84:00:00:01`):

```
nmcli connection add type ethernet ifname enx028484000001 con-name ti84 ipv4.method shared
```

On Windows, enable Internet Connection Sharing for the new adapter.

## Status

All of it works on a TI-84 Plus (hardware revision 2, boot code 1.00)
connected to Linux: the OS (LCD, keypad, timer, power off), the USB driver
and CDC-NCM, ARP, IPv4, ICMP, UDP, TCP, DHCP, DNS and SNTP, TLS 1.3, the web
browser with forms, search and the Japanese font, and the SSH client
(tested against OpenSSH 9.6). Chinese, Hangul, Arabic and Devanagari text
and UTF-8 in the SSH terminal are tested in emu84.

## Using the calculator

The OS starts in the network console; **APPS** opens the menu (1 web
browser, 2 SSH client, 3 network console). **2nd+ON** turns the calculator
off, **ON** turns it on again.

### Typing

| Key | Effect |
|-----|--------|
| ALPHA | cycles the mode: `1` digits, `A` upper case, `a` lower case (shown in the status line) |
| letters | the green ALPHA labels (MATH = A ... 2 = Z); 0 is a space, `.` is `:`, `(-)` is `?`, `+` is `"`, 3 is `_` |
| digits mode | 0-9 `.` `,` `(` `)` `+` `-` `*` `/` `^`, STO is `=` |
| 2nd + key | one symbol: `,`→`;` `(`→`{` `)`→`}` `*`→`[` `-`→`]` `/`→`\|` `^`→`~` `+`→`'` `.`→`_` `0`→`` ` `` STO→`=` LOG→`<` LN→`>` x²→`\` x⁻¹→`$` MATH→`!` APPS→`@` PRGM→`#` SIN→`%` COS→`&` TAN→`*` `(-)`→`?` |
| DEL / CLEAR | delete a character / clear the line (CLEAR on an empty line cancels) |
| LEFT/RIGHT, UP/DOWN | move the cursor, start/end of the line |

## Security

This is an experiment tuned for speed; **the connections are not secure**:

- TLS: no certificate, CertificateVerify or Finished checks, received
  record tags are not checked, and the X25519 private key is 1
  (`KX_INSECURE_FAST`), so anybody who records the traffic can decrypt it.
- SSH: the host key and its signature are not checked, received MACs are
  not verified.
- Random numbers come from xorshift32 (`RNG_INSECURE_FAST`).
- Received IP/TCP/UDP checksums are not checked (USB has its own CRC).

## Speed

15 MHz, measured in emu84:

| | |
|---|---|
| a web page over HTTP (network stack and the browser's renderer) | about 9 KB/s |
| AES-128 / ChaCha20 decryption (Z80 assembly) | 12.3 / 7.3 KB/s |
| SHA-256 (Z80 assembly) | 3.4 KB/s |
| TLS 1.3 full handshake | about 2 s of CPU time after the ServerHello (resumed: 1.3 s) |

## Building

Requirements: **SDCC 4.4.0** (4.5.0 miscompiles variable shifts), Python 3,
GCC (emulator, host tests), `patch`, and
[mktiupgrade](https://github.com/KnightOS/mktiupgrade).

```
make                      # build-84p/lwip84.8xu (OS upgrade) and lwip84.rom
make MODEL=84pse          # TI-84 Plus Silver Edition
make DEBUG=1              # debug build: log on the LCD (or in emu84)
make emu                  # the emulator, build-84p/emu84
make FONT=other.txt       # other external fonts (FONT= for none)
```

The Makefile looks for SDCC in `../tools/sdcc-4.4.0/bin` and other tools in
`../tools/local/bin`; override with `SDCC_BIN=` and `TOOLS=`.
`tools/get-key.sh` downloads TI's public OS signing key `0A` into `keys/`.

SDCC 4.4.0 sometimes loses the address of an array in `a[i++] = v` when
`a` is reached through a pointer and `i` is a static variable;
`tools/check_iy.py` looks for that in every module and stops the build.

Every push to `main` builds both models, runs the tests and publishes the
`.8xu` and `.rom` files as a GitHub release (`.github/workflows/build.yml`).

## Installing

Back up the calculator first: TI-OS and RAM are replaced. Then send the
`.8xu` as an OS upgrade from the boot code: remove a battery, hold DEL while
putting it back, plug in the USB cable when "Waiting..." appears, and send
the file within about 50 seconds (TiLP, TI Connect, or
`ti84link sendos FILE.8xu --force`, retried every few seconds because the
boot code answers slowly). Boot code 1.03 and newer need a workaround such
as UOSRECV, as for KnightOS.

On Linux, `tools/flash-calc.sh` does it all (backup with a ROM dump, send,
wait for the network); `sudo tools/host-setup.sh` once installs the udev
rules and the NetworkManager profile. `tools/build-ti84link.sh` builds
`ti84link` against a locally unpacked libticalcs.

To go back to TI-OS, send TI's OS upgrade the same way.

## How it works

### Memory and code banking

```
0000-3FFF  flash page 0: reset, interrupts, far-call thunks, constants, USB FIFO code
4000-7FFF  bank A: the flash page of the running code
8000-FFFF  RAM: static data (12 KiB, mostly lwIP pools), heap (17 KiB), stack (2 KiB)
```

`tools/banker.py` spreads the functions over flash pages and rewrites the
SDCC output: calls to another page and function pointers go through a
4-byte thunk in page 0 (`rst 10h`, page, address) that switches bank A and
keeps the return information on a far stack. Constants named `*_bk` (font,
tables) stay in flash next to the function that uses them. The boot code
does not run code on pages 08h-29h (08h-69h on the SE), so code is on pages
01-07 and 33-39 plus the crypto page 3B (73-79 and 7B on the SE). Page 3Ah
(7Ah) is left alone, the boot code keeps the OS header there. Pages 08h-29h
hold data: the external font from 08h on, the rest is free.

The six other RAM pages (96 KiB) are reached through `xram_read` /
`xram_write` and hold the web page (64 KiB), the SSH scrollback (24 KiB),
the console history and the browser history.

### USB

`os/usb.c` drives the MUSBFDRC controller as a device with one CDC-NCM
function (`os/ncm.c`, NTB16); endpoint 0 is served from the interrupt
handler. VBus has to read low for half a second before the configuration
is dropped, and if the host has not configured the function again three
seconds after that while VBus is present, the OS disconnects and connects
again so that the host enumerates it anew (emu84: `vbusglitch MS`).

### Network library

| Path | Contents |
|------|----------|
| `lib/net/netif_ncm.c`, `lib/net/net84.c` | lwIP interface, start-up, DHCP, main-loop service |
| `lib/net/sock84.c` | sockets for the applications |
| `lib/net/tls84.c` | TLS 1.3 client as an lwIP altcp layer |
| `lib/net/kx84.c` | the X25519 key pair |
| `include/lwipopts.h` | lwIP configuration |
| `patches/` | lwIP fix: stop the TCP persist timer when nothing is left to send |

`lib/net/sock84.h` is what the applications use (see the
[network console](app/console/README.md#the-socket-api)):

```
struct sock *s = sock_new(SOCK_TLS, 120000, event_cb);   /* or SOCK_TCP */
sock_connect(s, "example.com", 443);   /* waits for DHCP, resolves, connects */
sock_write(s, data, len);              /* queued if TCP cannot take it */
n = sock_read(s, buf, sizeof(buf));    /* from the main loop */
```

Received data stays in lwIP's pbufs until it is read, and the TCP window
opens as it is read. Events come from the main loop, never from inside
lwIP.

TLS 1.3 uses X25519 and `TLS_AES_128_GCM_SHA256`, which every TLS 1.3
server must support; a host that refuses it gets
`TLS_CHACHA20_POLY1305_SHA256` on the next attempt. Records are decrypted
in place as a stream, so 16 KiB records need no 16 KiB buffer. Session
tickets are kept for the last host and used for PSK resumption. No
HelloRetryRequest, early data or KeyUpdate.

AES, GHASH, ChaCha20, Poly1305, SHA-256 and the X25519 field arithmetic are
Z80 assembly produced by `tools/gen_*_z80.py` on one fixed flash page (3Bh,
7Bh on the SE); the rest is C (`lib/crypto/`). AES suits an 8-bit CPU better
than ChaCha20 (bytes and a table instead of 32-bit additions and rotations).

### External font

Characters that cannot be shown in ASCII are drawn with bitmap fonts on the
flash data pages (08h-10h, 147 KB). `fonts/fonts.txt` lists them in
priority order:

| Font | Scripts |
|------|---------|
| [k6x8](https://littlelimit.net/k6x8.htm) (6x8) | Japanese (JIS levels 1 and 2), kana, Greek, Cyrillic, symbols |
| [QuanPixel](https://diaowinner.itch.io/galmuri-extended) (8x8) | Hangul (4363 syllables), the Han characters of GB 2312 and Big5 level 1 that JIS lacks, bopomofo |
| [unscii](http://pelulamu.net/unscii/) Arabic (8x8) | Arabic with its contextual forms (missing letters made from similar ones) |
| [GNU Unifont](https://unifoundry.com/unifont/) Devanagari, halved to 8x8 | Devanagari |

- `tools/mkfont84.py` merges them (15877 glyphs): a table by the high byte
  of the code point, the sorted low bytes, 2 bits per glyph (half width,
  full width, mark drawn over the glyph before it) and 8 bytes of pixels
  per glyph, and a checksum.
- `tools/mkrom.py` puts the blob on the data pages and adds them to the
  `.8xu`. mktiupgrade adds the first page of every 64 KB flash sector, and
  the boot code erases a sector when it receives that page, so the data
  pages are written cleanly.
- `lib/font/font.c` checks the checksum at start-up (the console's status
  shows `font ok`; without a valid font such characters stay `?`) and finds
  a glyph by binary search, with a cache of recent characters.
- In text a glyph takes two cells: a lead byte (01h-1Fh, 7Fh, 80h-9Fh or
  FFh) and a trail byte 01h-FFh, which together give the glyph number (up to
  16575), so text stays a plain string and ASCII keeps its meaning. A row
  with glyphs is 8 pixels high, other rows stay 6 pixels (`os/ui.c`).

Arabic shaping, right-to-left runs and Devanagari reordering are done by the
applications that lay out text (see the [browser](app/browser/README.md)).

## Testing

`emu84` runs the OS image headless, including the boot code's execution
protection, with a virtual USB host that speaks CDC-NCM and bridges the
frames to [passt](https://passt.top) for real network access without root:

```
tools/run-passt.sh /tmp/passt.sock &          # calculator 192.168.84.2, PC 192.168.84.1
build-84p/emu84 -d -u -n /tmp/passt.sock build-84p/lwip84.rom <<EOF
run 3000
key trace
run 6000
screen
EOF
```

Commands: `run MS`, `waitlog TEXT MS`, `key NAME`, `down`/`up NAME`, `on`,
`vbusglitch MS`, `screen [FILE.pbm]`, `regs`, `dumpram FILE`,
`profile on|FILE`, `quit`. The PC's `127.0.0.1` is `192.168.84.1` for the
calculator. `tools/typekeys.py [--mode a] TEXT` turns text into `key`
commands, `tools/pbm2png.py` converts screens.

Host tests of the library (also run by CI; the browser has its own in
`app/browser/tests/`):

```
gcc -O2 -o ../tmp/z80run tests/z80run.c
python3 tests/asm_test.py aes 30           # also sha256, poly1305, ghash: Z80 code vs Python
gcc -O2 -I lib/crypto -I tests -o ../tmp/aes_test tests/aes_test.c lib/crypto/aes.c && ../tmp/aes_test
gcc -O2 -I lib/crypto -o ../tmp/crypto_test tests/crypto_test.c lib/crypto/sha256.c lib/crypto/chacha.c lib/crypto/x25519.c && ../tmp/crypto_test
```

## License

BSD (see `COPYING`); lwIP is BSD, `emu/third_party/z80.h` is from
[floooh/chips](https://github.com/floooh/chips) (zlib), the fonts in
`fonts/` keep their licenses: k6x8 by Num Kadoma (free to use, copy and
modify, `fonts/k6x8/k6x8.txt`), QuanPixel (SIL OFL 1.1), unscii (CC0) and GNU
Unifont (SIL OFL 1.1 or GPL 2+ with the font exception).
