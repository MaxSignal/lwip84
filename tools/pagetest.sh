#!/bin/sh
# pagetest.sh PAGE... - the debug build plus a test mark ("PG", page, then
# the page number repeated) on each PAGE, as an OS upgrade in
# build-debug-84p/pagetest.8xu.  After sending it, Y= lists the marked pages
# found on flash: the ones the boot code wrote.
set -e
B=build-debug-84p
PATH=$(cd .. && pwd)/tools/local/bin:$PATH
make DEBUG=1 >/dev/null
python3 - "$B/lwip84.rom" "$B/pagetest.rom" "$@" <<'PY'
import sys
rom = bytearray(open(sys.argv[1], 'rb').read())
for a in sys.argv[3:]:
    p = int(a, 16)
    assert all(b == 0xFF for b in rom[p * 16384:(p + 1) * 16384]), 'page %02X in use' % p
    rom[p * 16384:(p + 1) * 16384] = (b'PG' + bytes([p]) * 16382)
open(sys.argv[2], 'wb').write(rom)
PY
mktiupgrade -p -k keys/0A.key -d TI-84+ -v 1.0 $B/pagetest.rom $B/pagetest.8xu $(cat $B/pages.txt) "$@"
ls -l $B/pagetest.8xu
