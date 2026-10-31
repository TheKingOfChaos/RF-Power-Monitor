#!/bin/bash
# DHCPv6 test network in WSL (run as root, mirrored networking): a unicast router
# advertisement sender and Kea that drops every packet not sent from the device's
# link-local address and gives addresses only to the device's DUID.
#   testnet.sh start <iface> <device MAC> <device link-local> <M> <O> <subnet/64> <pool> <valid> <preferred> <renew> <rebind>
#   testnet.sh stop | log | ralog
# Mirrored networking gives eth0 its link-local address without a fe80::/64 route, so
# Kea could not reply to the device; the route is added while the test network runs.
set -u
DIR=/tmp/rfups-dhcp6
HERE=$(cd "$(dirname "$0")" && pwd)

stop() {
    for name in ra kea; do
        if [ -f "$DIR/$name.pid" ]; then
            kill "$(cat "$DIR/$name.pid")" 2>/dev/null
            rm -f "$DIR/$name.pid"
        fi
    done
    # Kea leaves its own pid file; a stale one stops the next start
    rm -f /run/kea/kea-dhcp6.kea-dhcp6.pid
    if [ -f "$DIR/route" ]; then
        ip -6 route del fe80::/64 dev "$(cat "$DIR/route")" 2>/dev/null
        rm -f "$DIR/route"
    fi
}

case "${1:-}" in
start)
    shift
    IFACE=$1 MAC=$2 LL=$3 M=$4 O=$5 SUBNET=$6 POOL=$7 VALID=$8 PREF=$9 RENEW=${10} REBIND=${11}
    stop
    mkdir -p "$DIR" /run/kea
    rm -f "$DIR"/*.log "$DIR"/kea.conf
    DUID="00030001$(echo "$MAC" | tr -d ':-' | tr 'A-F' 'a-f')"
    DEVLLHEX=$(python3 -c "import ipaddress, sys; print(ipaddress.IPv6Address(sys.argv[1]).packed.hex())" "$LL")
    DNS=$(ip -6 addr show dev "$IFACE" scope link | awk '/inet6/ {split($2, a, "/"); print a[1]; exit}')
    if ! ip -6 route show dev "$IFACE" | grep -q '^fe80::/64'; then
        ip -6 route add fe80::/64 dev "$IFACE" && echo "$IFACE" > "$DIR/route"
    fi
    sed -e "s|@IFACE@|$IFACE|g" -e "s|@DUID@|$DUID|g" -e "s|@DEVLLHEX@|$DEVLLHEX|g" -e "s|@SUBNET@|$SUBNET|g" -e "s|@POOL@|$POOL|g" \
        -e "s|@VALID@|$VALID|g" -e "s|@PREFERRED@|$PREF|g" -e "s|@RENEW@|$RENEW|g" -e "s|@REBIND@|$REBIND|g" \
        -e "s|@DNS@|${DNS:-fe80::1}|g" "$HERE/kea-dhcp6.conf.in" > "$DIR/kea.conf"
    nohup kea-dhcp6 -c "$DIR/kea.conf" > "$DIR/kea.log" 2>&1 &
    echo $! > "$DIR/kea.pid"
    nohup python3 "$HERE/send_ra.py" "$IFACE" "$MAC" "$LL" "$M" "$O" > "$DIR/ra.log" 2>&1 &
    echo $! > "$DIR/ra.pid"
    sleep 2
    for name in kea ra; do
        kill -0 "$(cat "$DIR/$name.pid")" 2>/dev/null || { echo "$name did not start:"; cat "$DIR/$name.log"; stop; exit 1; }
    done
    echo "started (DUID $DUID)"
    ;;
stop) stop; echo stopped ;;
log) cat "$DIR/kea.log" 2>/dev/null ;;
ralog) cat "$DIR/ra.log" 2>/dev/null ;;
*) echo "usage: $0 start ...|stop|log|ralog"; exit 2 ;;
esac
