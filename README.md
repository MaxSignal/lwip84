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
| TLS / WSS | not available (see below) |

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

The OS and the stack are about 150 KiB of Z80 code plus about 12 KiB in page 0. `tools/banker.py` distributes the
functions of all modules over flash pages and rewrites the SDCC assembler
output: calls inside a page stay direct, while calls to another page and all
function pointers go through a 10-byte thunk in page 0. The thunk switches
bank A and keeps the return information on a separate far stack, so the
normal SDCC calling convention keeps working. Constants are compiled into
page 0 (`--constseg CONST`) so that pointers to them are valid everywhere.

The boot code forbids executing code on flash pages 08h-29h (08h-69h on the
SE), so code is placed on pages 01-07 and 33-3B (73-7B on the SE), the same
pages TI-OS uses.

### Source layout

| Path | Contents |
|------|----------|
| `os/` | the OS: start-up, interrupts, far calls, LCD console, keypad, heap, USB driver (`usb.c`), CDC-NCM (`ncm.c`), test console (`main.c`, `apps.c`, `demo.c`) |
| `port/` | lwIP-CE platform layer: `sys_arch`, NCM netif, USB Ethernet driver entry points, SNTP clock, TLS stub |
| `include/` | `lwipopts.h`, `arch/cc.h` and small libc additions for SDCC |
| `emu/` | `emu84`, a headless emulator with a USB host model |
| `tools/` | `banker.py`, `mkrom.py`, profiling and helper scripts |

The shared lwIP-CE sources needed only small changes, all guarded by
`LWIP_PORT_TI84P` or portable: `core/init.c` (heap size), `lwip-imports.h`
(no usbdrvce table), `lwIP.c` (driver header), `app_config.h` (overridable
TLS floor), `altcp_ws.c` (no variable-length array).

## Test console

The OS currently starts a test console:

| Key | Action |
|-----|--------|
| Y= | status: USB registers, NTB counters, gateway, DNS, free heap |
| WINDOW | ping the gateway |
| ZOOM | DNS lookup of example.com |
| TRACE | HTTP GET http://example.com/ through `lwip_socket_*` |
| GRAPH | TCP echo test with gateway:8084 |
| VARS | download test from gateway:8085 (prints bytes/s) |
| MODE / STAT | SNTP time sync / show the time |
| CLEAR | clear the screen |
| 2nd, ON | power off (ON wakes the calculator) |

Applications are linked into the OS and use the same `lwIP.h` API as on the
CE; see `os/apps.c`.

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

## Why there is no TLS

lwIP-CE's TLS 1.3 stack implements AES/GHASH, SHA-256, RSA and X25519 in eZ80
assembly. In C on a 15 MHz Z80 an X25519 key exchange takes tens of seconds
and an RSA-2048 signature check about as long again, so a handshake would take
minutes. TLS records of up to 16 KiB also exceed the 32 KiB data window that
the stack, its heap and the USB buffers share. `port/tls_stub.c` makes
TLS and WSS sockets fail cleanly. A Z80 assembly port of the primitives, with
record buffers in the spare RAM pages 82h-87h, would be the way to add it.

## Third-party code

- `emu/third_party/z80.h`: Z80 emulator from [floooh/chips](https://github.com/floooh/chips), zlib license
- passt and mktiupgrade are used as external tools.
