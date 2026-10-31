# DHCPv6 fixtures

`test/native/dhcp6_frames/frames.h` holds real DHCPv6 frames for the native tests of `lib/dhcp6_client`. They come from two captures:

- `lab.pcap`: `capture.sh` builds a private lab of Linux network namespaces in WSL (nothing reaches the LAN): Kea 2.4.1 with one pool address and short timers, dhcpcd 10.0.6 as the client (DUID-LL), and `test/hil/dhcp6/send_ra.py` announcing the M flag. Client 1 gets the address and renews it; client 2 gets an Advertise with NoAddrsAvail.
- `spike.pcap`: the HIL test network (`test/hil/test_dhcp6_testnet.py`): the device's own Information-request (lwIP's stateless client) and Kea's reply.

Regenerate (needs root in WSL for the namespaces, and `kea-dhcp6-server`, `python3-scapy`, `dhcpcd-base` there):

```powershell
wsl -d Ubuntu -u root -- bash scripts/dhcp6_capture/capture.sh
wsl -d Ubuntu -- python3 scripts/dhcp6_capture/extract.py
```

Frames are kept byte for byte, except that a frame captured on the sending side of a virtual link has its UDP/ICMPv6 checksum completed (the kernel left it for checksum offload); the frame's comment in `frames.h` says so.

`capture.sh lan` sends one Router Solicitation on WSL's `eth0` to capture the LAN router's advertisement. With WSL in mirrored mode no router advertisement reaches Linux, so the fixtures use `send_ra.py` without the M flag for an ordinary router's flags instead.
