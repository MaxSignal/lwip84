# Platforms

The kernel (`kernel/`), the network library (`lib/`) and the applications
(`app/`) are the same everywhere. What a platform adds is how the program
starts, how it calls code on other flash pages and its `main`:

| Directory | Build | Result |
|-----------|-------|--------|
| `os/` | `make` (`MODEL=84pse`, `RAM48=1`, `DEBUG=1`) | the standalone OS: `.8xu`, sent from the boot code instead of TI-OS |
| `tios/` | `make PLATFORM=tios APP=browser` or `APP=ssh` | a TI-OS Flash application: `.8xk` |

Each platform has `crt0.s` (start-up, interrupt entry), `farcall.s` (the
far-call trampoline used by the thunks of `tools/banker.py`) and `main.c`.

## The standalone OS (`os/`)

Flash page 0 holds the start-up, the interrupt path, the far calls and all
constants; the banked code is on pages 01h-07h and 33h-39h (73h-79h on the
SE), the fonts and dictionaries on data pages (see the main README). `main.c`
is the menu (APPS), the network console and the switching between the
applications.

## The TI-OS applications (`tios/`)

For the TI-84 Plus with 128 KiB of RAM and TI-OS. The web browser
(`lwIPweb`, 12 pages) and the SSH client (`lwIPssh`, 11 pages) are separate
applications, so that each has as much RAM as possible. They have no
external font (non-ASCII text shows as `?`, Cyrillic and Greek still work
in SSH) and the buffers of the 48 KiB build (a page keeps 288 lines). MODE
(2nd+MODE in SSH) goes back to TI-OS.

TI-OS keeps flash page 0, so the program has 32 KiB of RAM for everything
that has to be reachable from every page:

```
0000-3FFF  TI-OS flash page 0
4000-7FFF  bank A: the app's flash page of the running code
8000-8183  IM 2 table (I = 80h) and the jump to the interrupt entry
8184-      common code, thunks, constants, initializers (the RAM image)
  ...      data (lwIP pools, USB buffers, ...), heap (8.7 KiB in the browser)
FA00-FFFF  stack
```

TI-OS lets code run in RAM only on page 81h (8000h-BFFFh), so the code
comes first. `launch.s` is the app's first page: the header, the start at
4080h and the way back.

- Start: interrupts off; if pages 86h and 87h are one page (48 KiB of RAM)
  back to TI-OS at once; save TI-OS's RAM pages 81h and 80h in 87h and 86h,
  its stack pointer, I and the ports 03h, 04h, 05h, 07h and 20h; copy the
  RAM image from the app's first page to 8184h; `crt0.s` sets up IM 2,
  clears the data and calls `main`.
- Exit (`__app_exit`): stop the USB controller, then on the app's first
  page: copy 87h and 86h back, restore the ports, the stack pointer, I and
  IM 1, the LCD contrast and mode of TI-OS, and `B_JUMP _JForceCmdNoChar`.
- Far calls: TI-OS owns `RST 10h`, so a thunk is `CALL ___farcall_rst`
  (6 bytes, `banker.py --call-thunks`), and the page in it is the app's
  logical page n, which TI-OS put on flash page (first page - n).
  `farcall.s` does the subtraction.
- Extended RAM: page 84h (16 KiB); 82h and 83h are left to TI-OS.

`tools/mkapp84.py` puts the pages together (the image at 4400h of the first
page, its length at 4083h, the name and page count in the header) and
`rabbitsign` signs it with the "shareware" key 0104 (`tools/local/bin`,
built from <https://github.com/abbrev/rabbitsign>).

Send the `.8xk` to TI-OS with TiLP, TI Connect or `ti84link sendapp FILE`;
it appears under APPS. After the app, TI-OS may need the USB cable plugged
in again before it talks to a computer.

### Testing

```
make PLATFORM=tios APP=browser DEBUG=1 emu
build-debug-tios-browser/emu84 -app -d -u -n /tmp/passt.sock build-debug-tios-browser/lwip84.bin
```

`-app` loads the pages as TI-OS would (first page 1Ch, then downwards),
puts a stand-in for TI-OS on page 0 that starts the app, fills TI-OS's RAM
with a pattern and checks it when the app goes back ("RAM restored"), and
faults on code run from RAM outside TI-OS's limits.
