#!/bin/sh
# run-passt.sh SOCKET [PCAP] - start passt for emu84 (calculator 192.168.84.2, gateway/host 192.168.84.1)
sock=${1:?socket path}
rm -f "$sock"
exec passt -f -s "$sock" -a 192.168.84.2 -n 24 -g 192.168.84.1 --map-host-loopback 192.168.84.1 \
    ${2:+-p "$2"} >/dev/null 2>&1
