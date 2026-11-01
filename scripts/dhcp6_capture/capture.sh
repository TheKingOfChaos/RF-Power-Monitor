#!/bin/bash
# Capture real DHCPv6 exchanges for the native tests (test/native/dhcp6_frames/frames.h).
# Runs in WSL as root: wsl -d Ubuntu -u root -- bash scripts/dhcp6_capture/capture.sh
#
# A lab of network namespaces on a private bridge (nothing reaches the LAN):
#   d6kea  Kea 2.4 (Ubuntu kea-dhcp6-server), fd00:d6::/64, ONE pool address, short timers,
#          and test/hil/dhcp6/send_ra.py announcing the M flag to each client
#   d6c1   dhcpcd 10 with DUID-LL: Solicit, Advertise, Request, Reply, Renew, Reply
#   d6c2   dhcpcd 10, started after d6c1 holds the only address: Advertise with NoAddrsAvail
# Optionally (argument "lan"), one Router Solicitation on WSL's eth0 to capture the LAN
# router's own advertisement; routers answer those all the time, nothing changes.
# Output: scripts/dhcp6_capture/lab.pcap (and lan_ra.pcap); then run extract.py.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
RA=$HERE/../../test/hil/dhcp6/send_ra.py
WORK=/tmp/rfpower-d6lab
MAC_KEA=02:00:00:00:d6:00
MAC_C1=02:00:00:00:d6:01
MAC_C2=02:00:00:00:d6:02

cleanup() {
    pkill -f "kea-dhcp6 -c $WORK" 2>/dev/null
    pkill -f "send_ra.py c[12]" 2>/dev/null
    pkill -f "tcpdump -i d6br" 2>/dev/null
    # dhcpcd renames its processes ("dhcpcd: c1 [ip6]", "[DHCP6 proxy] fe80::..."); a survivor
    # would keep its deleted namespace alive and keep talking to the next run's Kea
    pkill -f 'dhcpcd: .*(c1|c2|fe80::ff:fe00:d60|fd00:d6::)' 2>/dev/null
    sleep 1
    for ns in d6kea d6c1 d6c2; do ip netns del $ns 2>/dev/null; done
    for l in vd6kea vd6c1 vd6c2 d6br; do ip link del $l 2>/dev/null; done
    rm -f /run/kea/kea-dhcp6.kea-dhcp6.pid
    # dhcpcd keeps one DUID file for all instances; put back whatever was there
    if [ -f "$WORK/duid.saved" ]; then mv "$WORK/duid.saved" "$DUID_FILE"; else rm -f "$DUID_FILE"; fi
    rm -f /var/lib/dhcpcd/c1.lease6 /var/lib/dhcpcd/c2.lease6
}
DUID_FILE=/var/lib/dhcpcd/duid
trap cleanup EXIT

if [ "${1:-}" = "lan" ]; then
    timeout 15 tcpdump -i eth0 -n -w "$HERE/lan_ra.pcap" "icmp6 and ip6[40] == 134" &
    sleep 2
    python3 -c "from scapy.all import *; send(IPv6(dst='ff02::2', hlim=255)/ICMPv6ND_RS(), iface='eth0', verbose=False)"
    wait
    tcpdump -n -r "$HERE/lan_ra.pcap" 2>/dev/null | head -3
    trap - EXIT
    exit 0
fi

cleanup
rm -rf "$WORK" && mkdir -p "$WORK" /run/kea
[ -f "$DUID_FILE" ] && cp "$DUID_FILE" "$WORK/duid.saved"
ip link add d6br type bridge && ip link set d6br up
port() {  # namespace, interface name inside, MAC
    ip netns add "$1"
    ip link add "v$1" type veth peer name "$2" netns "$1"
    ip link set "v$1" master d6br up
    ip -n "$1" link set lo up
    ip -n "$1" link set "$2" address "$3" up
}
port d6kea k0 $MAC_KEA
port d6c1 c1 $MAC_C1
port d6c2 c2 $MAC_C2
ip -n d6kea addr add fd00:d6::1/64 dev k0
sleep 3  # duplicate address detection on the link-local addresses

cat > "$WORK/kea.conf" <<EOF
{ "Dhcp6": {
  "interfaces-config": { "interfaces": [ "k0" ] },
  "lease-database": { "type": "memfile", "persist": false },
  "server-id": { "type": "LL", "htype": 1, "identifier": "020000000d600" },
  "valid-lifetime": 40, "preferred-lifetime": 20, "renew-timer": 6, "rebind-timer": 12,
  "subnet6": [ { "id": 1, "subnet": "fd00:d6::/64", "interface": "k0",
                 "pools": [ { "pool": "fd00:d6::100 - fd00:d6::100" } ] } ],
  "loggers": [ { "name": "kea-dhcp6", "output_options": [ { "output": "$WORK/kea.log" } ], "severity": "INFO" } ]
} }
EOF
sed -i 's/"020000000d600"/"02000000d600"/' "$WORK/kea.conf"

cat > "$WORK/dhcpcd.conf" <<EOF
duid ll
ipv6only
ia_na 1
fqdn both
hostname lab-client
nohook resolv.conf, timesyncd, hostname, ntp
EOF

tcpdump -i d6br -n -w "$HERE/lab.pcap" "udp port 546 or udp port 547 or (icmp6 and ip6[40] == 134)" &
sleep 1
ip netns exec d6kea kea-dhcp6 -c "$WORK/kea.conf" &
sleep 2
ll() { ip -n "$1" -6 addr show dev "$2" scope link | awk '/inet6/ {split($2, a, "/"); print a[1]; exit}'; }
ip netns exec d6kea python3 "$RA" k0 $MAC_C1 "$(ll d6c1 c1)" 1 0 2 > "$WORK/ra1.log" 2>&1 &
ip netns exec d6kea python3 "$RA" k0 $MAC_C2 "$(ll d6c2 c2)" 1 0 2 > "$WORK/ra2.log" 2>&1 &
# One advertisement without M (and without O), as on an ordinary LAN, for the parser's "no" case
ip netns exec d6kea timeout 3 python3 "$RA" k0 $MAC_C2 "$(ll d6c2 c2)" 0 0 10 > /dev/null 2>&1 &

rm -f "$DUID_FILE"  # each client makes its own DUID-LL from its MAC
ip netns exec d6c1 dhcpcd -6 -B -f "$WORK/dhcpcd.conf" c1 > "$WORK/c1.log" 2>&1 &
sleep 15  # lease, then a Renew at T1 = 6 s
# One dhcpcd at a time (instances share /run/dhcpcd): c1 exits WITHOUT releasing, Kea keeps its lease
ip netns exec d6c1 dhcpcd -x c1 > /dev/null 2>&1
sleep 2
rm -f "$DUID_FILE"
ip netns exec d6c2 dhcpcd -6 -B -f "$WORK/dhcpcd.conf" c2 > "$WORK/c2.log" 2>&1 &
sleep 8   # the only address is taken: NoAddrsAvail
echo "--- kea"; grep -E "LEASE|NO_ADDR|FAIL|ERROR" "$WORK/kea.log" | cut -c1-200 | head
echo "--- c1"; head -12 "$WORK/c1.log"
echo "--- c2"; head -6 "$WORK/c2.log"
sleep 1
echo "--- capture"; tcpdump -n -r "$HERE/lab.pcap" 2>/dev/null | awk '{print $3, $4, $5, $6, $7, $8}' | sort | uniq -c
