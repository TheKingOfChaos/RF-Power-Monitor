"""Send router advertisements to ONE device only (unicast), for the DHCPv6 test network.

Runs in WSL as root (raw socket). The advertisement carries only the M and/or O
flag: router lifetime 0 (not a default router) and no prefix information, so the
device keeps its routes and SLAAC address, and no other host on the LAN sees it.

Usage: send_ra.py <interface> <device MAC> <device link-local> <M 0|1> <O 0|1> [interval s]
"""

import sys
import time

from scapy.all import Ether, ICMPv6ND_RA, ICMPv6NDOptSrcLLAddr, IPv6, get_if_hwaddr, in6_getifaddr, sendp


def main():
    iface, dev_mac, dev_ll, managed, other = sys.argv[1:6]
    interval = float(sys.argv[6]) if len(sys.argv) > 6 else 3.0
    if not dev_ll.lower().startswith("fe80:"):
        sys.exit("refusing: destination must be the device's link-local address, never multicast")
    pc_mac = get_if_hwaddr(iface)
    # The source must be our link-local address (RFC 4861); scapy has no route to pick it for fe80::
    pc_ll = next((a for a, _scope, i in in6_getifaddr() if i == iface and a.lower().startswith("fe80:")), None)
    if pc_ll is None:
        sys.exit("no link-local address on %s" % iface)
    ra = (Ether(src=pc_mac, dst=dev_mac.lower())
          / IPv6(src=pc_ll, dst=dev_ll, hlim=255)
          / ICMPv6ND_RA(M=int(managed), O=int(other), routerlifetime=0)
          / ICMPv6NDOptSrcLLAddr(lladdr=pc_mac))
    print("sending RA M=%s O=%s to %s (%s) every %.1f s" % (managed, other, dev_ll, dev_mac, interval), flush=True)
    while True:
        sendp(ra, iface=iface, verbose=False)
        time.sleep(interval)


if __name__ == "__main__":
    main()
