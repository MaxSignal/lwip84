#!/bin/sh
# flash-calc.sh - back up a TI-84 Plus / Plus SE running TI-OS and install
# lwIP-84+ on it.  No root needed (run tools/host-setup.sh once before).
#
#   tools/flash-calc.sh [--debug] [--no-romdump] [BACKUP_DIR]
#
# Steps: read OS/boot versions; save all variables and apps; dump the flash
# (ROM image, keeps TI-OS and the archive); refuse boot code >= 1.03; send the
# OS upgrade for the detected model; wait for the calculator's USB network.
set -e
here=$(cd "$(dirname "$0")" && pwd)
ti84p=$(cd "$here/.." && pwd)
PATH=$ti84p/../../tools/local/bin:$PATH
debug=; romdump=1; backup=
for a in "$@"; do
    case $a in
    --debug) debug=1 ;;
    --no-romdump) romdump= ;;
    *) backup=$a ;;
    esac
done
backup=${backup:-$ti84p/../../backup/$(date +%Y%m%d-%H%M%S)}
IFACE=enx028484000001

echo "== calculator"
info=$(ti84link info)
echo "$info"
model=$(echo "$info" | sed -n 's/^model: *//p')
boot=$(echo "$info" | sed -n 's/^boot: *//p')
case $boot in
1.00|1.01|1.02) ;;
*) echo "boot code $boot: the signed upgrade would be rejected (needs UOSRECV), stopping"; exit 2 ;;
esac

echo "== backup to $backup"
mkdir -p "$backup"
ti84link backup "$backup"
if [ -n "$romdump" ]; then
    echo "== ROM dump (keep the calculator on the home screen)"
    ti84link romdump "$backup/rom.$model.rom"
fi

build=$ti84p/build${debug:+-debug}-$model
echo "== building $build"
make -C "$ti84p" MODEL=$model ${debug:+DEBUG=1} >/dev/null
os=$build/lwip84.8xu
echo "== sending $os"
ti84link sendos "$os"

echo "== waiting for the calculator's USB network ($IFACE)"
i=0
while [ $i -lt 60 ] && ! ip link show "$IFACE" >/dev/null 2>&1; do sleep 1; i=$((i+1)); done
if ! ip link show "$IFACE" >/dev/null 2>&1; then
    echo "no $IFACE yet; USB devices:"; lsusb | grep -iE "1209:0001|0451:" || true
    exit 3
fi
ip -br addr show "$IFACE"
i=0
while [ $i -lt 30 ]; do
    calc=$(ip neigh show dev "$IFACE" | awk '/192\.168\.84\./ {print $1; exit}')
    [ -n "$calc" ] && break
    sleep 1; i=$((i+1))
done
if [ -n "$calc" ]; then
    echo "calculator at $calc"
    ping -c 3 "$calc" || true
else
    echo "the calculator has not taken a DHCP lease yet"
fi
