#!/usr/bin/env bash
# Re-apply host-side NAT so the r8q phone gets internet over the USB NCM gadget.
# Run after a HOST reboot (the rules are runtime). Phone side is persistent
# (networkd Gateway=172.16.42.14 + static /etc/resolv.conf).
set -e
UPLINK="${1:-$(ip route get 1.1.1.1 | grep -oP 'dev \K\S+')}"   # PC's internet iface
USBNET="${2:-$(for n in /sys/class/net/*; do d=$(basename "$(readlink -f "$n/device/driver" 2>/dev/null)"); case "$d" in cdc_ncm|cdc_ether|usbnet) basename "$n";; esac; done | head -1)}"
SUBNET=172.16.42.0/24
echo "uplink=$UPLINK usbnet=$USBNET"
sudo sysctl -w net.ipv4.ip_forward=1
sudo ip addr add 172.16.42.14/24 dev "$USBNET" 2>/dev/null || true
sudo ip link set "$USBNET" up
sudo iptables -t nat -C POSTROUTING -s $SUBNET -o "$UPLINK" -j MASQUERADE 2>/dev/null || sudo iptables -t nat -A POSTROUTING -s $SUBNET -o "$UPLINK" -j MASQUERADE
sudo iptables -C FORWARD -i "$USBNET" -o "$UPLINK" -j ACCEPT 2>/dev/null || sudo iptables -A FORWARD -i "$USBNET" -o "$UPLINK" -j ACCEPT
sudo iptables -C FORWARD -i "$UPLINK" -o "$USBNET" -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null || sudo iptables -A FORWARD -i "$UPLINK" -o "$USBNET" -m state --state RELATED,ESTABLISHED -j ACCEPT
echo "tethering NAT applied for $SUBNET out $UPLINK"
