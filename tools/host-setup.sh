#!/bin/sh
# host-setup.sh - one-time PC setup for lwIP-84+ (run as root: sudo tools/host-setup.sh)
#
#  * USB access for the calculator (TI vendor ID 0451, and the lwIP-84+ NCM
#    function 1209:0001) for the plugdev group
#  * a NetworkManager profile "ti84-usb" for the calculator's NCM interface
#    (enx028484000001): 192.168.84.1/24, DHCP + NAT ("shared"), activated
#    automatically whenever the calculator is plugged in
set -e
if [ "$(id -u)" != 0 ]; then
    echo "run as root: sudo $0" >&2
    exit 1
fi
IFACE=enx028484000001

rules=/etc/udev/rules.d/60-ti84plus.rules
cat > "$rules" <<'RULES'
# TI-84 Plus / Plus SE running TI-OS (TiLP / ti84link)
SUBSYSTEM=="usb", ATTRS{idVendor}=="0451", ATTRS{idProduct}=="e003", MODE="0664", GROUP="plugdev"
SUBSYSTEM=="usb", ATTRS{idVendor}=="0451", ATTRS{idProduct}=="e008", MODE="0664", GROUP="plugdev"
RULES
udevadm control --reload-rules
echo "installed $rules"

if nmcli -t -f NAME connection show | grep -qx ti84-usb; then
    nmcli connection delete ti84-usb >/dev/null
fi
nmcli connection add type ethernet con-name ti84-usb ifname "$IFACE" \
    connection.autoconnect yes connection.autoconnect-priority 100 \
    ipv4.method shared ipv4.addresses 192.168.84.1/24 ipv6.method disabled >/dev/null
echo "created NetworkManager connection ti84-usb for $IFACE (192.168.84.1/24, shared)"
