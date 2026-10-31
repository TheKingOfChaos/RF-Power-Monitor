# DHCPv6 address assignment (stateful DHCPv6 client)

Date: 2026-10-31. Status: design, not built. Reviewed by a builder check; its two questions are answered below (address slots: designer's call; test network: user's decision).

## Intent

Future-proofing. Today every network the device is on hands out IPv6 addresses by SLAAC, and the device already gets a SLAAC address, a link-local address and optionally a static one. If it is later placed on a network whose router says "take your address from DHCPv6" (the Managed flag in router advertisements), it should get a DHCPv6 address there too, without any configuration. Nobody needs this right now; the goal is that the device keeps working on such a network.

## Goals

- When a router advertisement has the M (Managed address configuration) flag, the device runs a stateful DHCPv6 client (RFC 8415) for one address (IA_NA) and uses that address next to its others.
- The full lease life cycle: Solicit, Advertise, Request, Reply; Renew at T1, Rebind at T2; the address is removed when its valid lifetime runs out; Decline when duplicate address detection fails. (Release only if the builder decides to stop the client when the M flag disappears, see Left to the builder; a reboot does not release, the server keeps the lease for the same DUID.)
- A stable client identity: DUID-LL (type 3) built from the device's MAC address, so a server recognises the device after restarts without anything being stored. IAID fixed (e.g. 1).
- The device sends its hostname (the one hostname that is sysName, DHCPv4 option 12 and mDNS) in the Client FQDN option (39), so a server can register it in DNS.
- The address is visible: status screen and web page list it marked `(dhcpv6)` with the remaining valid lifetime, like the static address is marked `(static)`. SNMP, the web page and mDNS work on it like on the other addresses (they already listen on all addresses).
- Retransmission per RFC 8415 section 15 (initial/maximum timeouts and counts per message type, with randomisation), so a missing or slow server never floods the network and never blocks anything.

## Non-goals

- Prefix delegation (IA_PD), temporary addresses (IA_TA), more than one IA_NA address, reconfigure (Reconfigure messages are ignored), authentication, rapid commit (may be requested later; not needed).
- A menu or web setting. The client follows the router's M flag; there is no on/off switch. (A setting can be added later if a real network needs it.)
- DHCPv6 over IPv4-only networks, relays-specific behaviour on the client side (relays are transparent to a client).
- DNS from DHCPv6 for the device's own use: the firmware resolves no names.
- Updating Arduino-Pico or lwIP: stays on the pinned platform (Arduino-Pico 5.4.3, lwIP 2.2.1). lwIP has no stateful DHCPv6 in any version, including its current development code, so the client is our own code.

## Anti-goals (a failure even if it "works")

- Anything that changes behaviour on networks without the M flag. SLAAC, link-local, the static IPv6 address, DHCPv4 and option 224 must behave exactly as now; all existing tests must still pass unchanged.
- Blocking core 0, or anything that pushes SNMP replies over 100 ms or eats core 0 stack (8 KB, shared with USB, lwIP and W5500 interrupts). No buffer over ~256 bytes on the stack.
- Taking an address without duplicate address detection, or keeping one after its valid lifetime.
- Parsing network input without bounds checks: every received message is untrusted.

## Constraints (found in the code)

- **lwIP owns UDP 546.** The core's `lwipopts.h` sets `LWIP_IPV6_DHCP6 1` unconditionally (it cannot be turned off with a build flag without a macro redefinition), and lwIP's stateless DHCPv6 binds a UDP pcb to port 546 on `IP6_ADDR_ANY` (`dhcp6.c`). A second pcb on 546 is not possible without `SO_REUSE`, and even then lwIP delivers a unicast datagram to one pcb only. lwIP's stateless client sends only Information-request and drops Replies whose transaction id is not its own.
- **lwIP announces the M and O flags.** `nd6.c` calls `dhcp6_nd6_ra_trigger(netif, managed, other)` for every router advertisement. lwIP is compiled from source in this build, so the call can be intercepted with `-Wl,--wrap=dhcp6_nd6_ra_trigger` (the DIAG build already uses `--wrap=panic`), forwarding to the original so the stateless client keeps working.
- **Address slots.** `LWIP_IPV6_NUM_ADDRESSES=4` (platformio.ini): link-local, SLAAC, static and DHCPv6 fill all four when the router announces one prefix. If no slot is free, the DHCPv6 address is not added and the status screen says why; it must never evict another address. The reverse is accepted too (decided by the designer, 2026-10-31): first come, first served. A DHCPv6 address in the last slot can keep a later SLAAC address (second prefix, renumbering) out; lwIP then skips that SLAAC address, and the status screen should make the full table visible. `LWIP_IPV6_NUM_ADDRESSES` stays 4, because raising it would change behaviour on networks without the M flag (anti-goal).
- **Lifetimes and DAD.** `LWIP_IPV6_ADDRESS_LIFETIMES` is on (follows autoconfig), so an address added with its valid and preferred lifetimes is expired by lwIP itself. lwIP runs duplicate address detection on tentative addresses and marks a duplicate invalid.
- **Existing pattern.** DHCPv4 option 224 is handled by a pure library (`lib/dhcp_option`) that builds/reads frames, called from the netif input/linkoutput hooks in `src/net_stats.cpp`, tested natively with frames captured from Kea 2.2.0 in Docker and from the LAN server.
- **Project rules** (`.clinerules`): protocol logic in a pure library under `lib/` with native tests; network-parsing code has truncation/corruption tests and passes `scripts/sanitize.sh`; protocol tests use captured bytes from real tools; on-device stack measurement for deep network paths; tests that need a specific network resource are opt-in via an environment variable.

## Decided

- Own client, pure library (working name `lib/dhcp6_client`): message encoding and decoding (options: Client ID, Server ID, IA_NA with IA Address, Elapsed Time, Option Request, Status Code, Client FQDN, Preference), and the client state machine with RFC 8415 timers, driven by "now" in milliseconds and incoming messages, emitting messages to send and address add/remove/decline actions. No lwIP or Arduino includes.
- Start on the M flag (via the wrapped `dhcp6_nd6_ra_trigger`), single IA_NA, DUID-LL from the MAC, hostname in option 39, no setting.
- Address added with the lifetimes from the server, after lwIP's DAD; Decline on duplicate; removed when the valid lifetime ends or the lease cannot be renewed/rebound.
- Visible on status screen and web page as `(dhcpv6)` with remaining lifetime; client state and counters (messages sent/received/ignored) on the status screen.
- Tests: native with messages captured from Kea (kea-dhcp6) plus every truncation and single-byte corruption, sanitizers; on-device stack measurement of the receive and send path; HIL on the test network below: the address appears, passes DAD, answers SNMP/ping6, is renewed (short lease in the test server), and disappears when the server stops answering past the valid lifetime; a duplicate (the test server hands out an address another host already uses) leads to a Decline.
- **The test network is part of this work** (decided by the user, 2026-10-31). The TODO item is only ticked when the HIL test runs and passes on it.
- **How the test network avoids disturbing the LAN** (designer's call, 2026-10-31; the builder confirms it in a spike first): the PC sends router advertisements **unicast to the device's link-local address only**, with the M flag set, router lifetime 0 (not a default router) and no prefix information, so no other host on the LAN sees an M flag and the device's routes and SLAAC address stay as the LAN router made them. The test DHCPv6 server (Kea; real tool, as the project rules want) answers **only the device's DUID** (host reservation, unknown clients denied), with short lifetimes. Both run only while the HIL test runs, started and stopped by the test, and the test is opt-in by environment variable like the other network-resource tests. Fallback if the spike shows this cannot work from this PC (Docker Desktop and WSL may not pass multicast DHCPv6 or raw ICMPv6): an isolated link, a spare Ethernet port or USB adapter on the PC cabled to the device, with radvd and Kea bound to it; that needs the user's hardware and is asked for only then.

## Left to the builder

- **How messages get on and off the wire given that lwIP owns port 546.** Candidates: (a) the frame-hook pattern of option 224 for receiving (read DHCPv6 messages in the netif input hook, let lwIP drop them as foreign) plus building complete IPv6/UDP frames with source port 546 for sending through the link output; (b) a raw lwIP pcb or `udp_sendto_if_src` from an ephemeral port if the test server (Kea) and RFC 8415 allow replies to port 546 regardless of the client's source port (servers send to 546, but check); (c) `SO_REUSE` with careful pcb ordering. Pick the one that keeps lwIP's stateless client working and is testable; document why.
- Whether an Information-request is still needed when the O flag is set without M (lwIP's stateless client already does it: probably leave it alone).
- The exact retransmission constants (take them from RFC 8415 section 7.6), randomisation source, and how Renew/Rebind interact with lifetimes the server shortens.
- What happens when the M flag disappears from later advertisements: keep the lease until it expires (recommended, RFC 8415 does not require releasing) or Release.
- Where on the web page the address appears (next to the existing IPv6 list).
- Library and file names, counters, log wording.
- The mechanics of the test network: where Kea and the RA sender run (WSL with mirrored networking, Docker with host networking in WSL, or a Windows-side sender such as scapy with Npcap), how the test finds the device's link-local address and DUID (status screen / MAC), and how it starts and stops them. Do the feasibility spike before the client is built, so a dead end shows up early. Never send an RA with the M flag to the LAN's multicast address, and never leave the test server running after the test.
