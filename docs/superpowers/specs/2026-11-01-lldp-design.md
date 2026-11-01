# LLDP: announce the device and show the switch port it is plugged into

Date: 2026-11-01. Status: draft.

## Intent

The user wants the RF-UPS-Monitor to appear in their switch's LLDP neighbour list
(a Juniper EX2200-C), so it is clear which device sits on which port. As a wanted
extra, the device also listens to the switch's LLDP and shows which switch and
port it is plugged into, on the serial menu's status screen and on the web page.

Success: `show lldp neighbors` on the EX2200-C lists the device by its hostname
on the right port, and the device's status screen and web page name the switch
and its port.

## Goals

1. **Send** (IEEE 802.1AB): an LLDPDU to 01:80:C2:00:00:0E, EtherType 0x88CC,
   from the device's MAC, every 30 s, plus one soon after the link comes up and
   one whenever the information in it changes (an address gained or lost, the
   hostname changed), at most one per second (our call; 802.1AB sends on
   local change, and DHCP finishes after the link is up). No TTL 0 frame before
   a reboot or factory reset (our call: the switch forgets us within 120 s).
   TTL 120 s. Mandatory TLVs plus:
   - Chassis ID: subtype 4 (MAC address).
   - Port ID: subtype 3 (MAC address), as Linux lldpd does on a single-port host
     (our call).
   - TTL: 120.
   - Port Description: the IF-MIB ifDescr, "W5500 10/100 Ethernet".
   - System Name: the hostname in use (= sysName, the DHCP/mDNS name).
   - System Description: the same text as sysDescr (includes the firmware version).
   - System Capabilities: station only (bit 7), enabled station only.
   - Management Address: the IPv4 address if there is one, and the global IPv6
     address if there is one (one TLV each), interface numbering ifIndex 1,
     no OID. None when the device has no address yet.
   - End of LLDPDU.
2. **Listen**: decode LLDPDUs that arrive (they reach the input hook already;
   see Constraints) and keep one neighbour: system name, port ID, port
   description, chassis ID, and the first management address (IPv4 or IPv6).
   The neighbour is forgotten when its TTL runs out, when a TTL 0 (shutdown)
   LLDPDU arrives, or when the link goes down.
3. **Show** the neighbour:
   - Serial menu status screen: a line `Switch: <system name>, port <port>`
     (`Switch: none seen` when there is none).
   - Web page: a "Switch" row in the network section with the same text plus
     the management address when known.
   - Which port text: Junos sends the Port ID as a locally assigned ifIndex
     (subtype 7, e.g. "513") and the interface name in Port Description
     (e.g. "ge-0/0/5.0"). Show the Port Description when the Port ID is subtype
     7 (locally assigned) and a description is present; otherwise the Port ID
     (interface name, MAC as aa:bb:.., or text). The other fields are as received.
   - If the switch sends no System Name, show its Chassis ID instead.

## Non-goals

- No LLDP-MIB over SNMP (lldpRemTable, lldpLocalSystemData). LibreNMS already
  learns the neighbour from the switch's own LLDP-MIB.
- No settings: LLDP is always on, no menu option to turn it off or change timers.
- No LLDP-MED, 802.1 or 802.3 organisational TLVs sent. Received ones are
  skipped.
- No more than one neighbour (the device has one port; if a second sender is
  seen, the latest one wins, keyed by chassis ID + port ID).
- No CDP.

## Anti-goals

- LLDP must never disturb SNMP, the web page, DHCP or IPv6: a malformed or hostile
  LLDPDU is dropped without crashing, and parsing adds no large stack buffers
  (core 0 rule: nothing over ~256 bytes on the stack).
- Text from the network is shown on the web page and the serial screen: it must
  be HTML-escaped on the page and stripped of control characters on both, and
  cut to a fixed length.

## Constraints

- The W5500 runs its MACRAW socket with MFEN and MMB clear (checked in the core's
  `libraries/lwIP_w5500/src/utility/w5500.cpp`, `setSn_MR(Sn_MR_MACRAW)`), so LLDP
  frames already reach `countingInput` in `src/net_stats.cpp`; lwIP drops
  EtherType 0x88CC. No driver change is needed.
- Sending goes through `netStatsSendFrame()` (counted like every other frame).
- Receiving: LLDP (EtherType 0x88CC) is no longer an unknown protocol. Today
  `lib/if_stats` (`classifyReceived`) counts it in ifInUnknownProtos; it moves
  to ifInMulticastPkts, since the device now uses it (our call, the IF-MIB
  meaning). Update `test/native/test_if_stats`, docs/MIB.md and README where
  they name LLDP as unknown, and check the HIL drop-counter test still holds.
- Build order: the receive hook and a hidden hex-dump command come first and
  are flashed, so the real EX2200-C frame can be captured before the parser
  tests are finished. Until then a hand-built Junos-style vector stands in.
- Pure logic in a new library `lib/lldp` (no Arduino includes): build an LLDPDU
  from a local-info struct into a caller buffer; parse a received frame into a
  neighbour struct; the neighbour table with TTL expiry and the display text
  (port choice above). Host tests in `test/native/test_lldp`, including
  truncated and corrupted frames, TLV lengths past the end, missing mandatory
  TLVs, a zero TTL, over-long strings, and a byte vector captured from a real
  EX2200-C. Added to `scripts/sanitize.sh`.
- Firmware glue in `src/lldp_service.cpp`: timer on core 0, inspect hook called
  from `countingInput`, link-down clears, getters for the menu and web.
- Project rules apply: TDD, `-Wall -Wextra -Werror`, docs (README, CHANGELOG,
  TODO, docs/TESTING.md), version bump (minor: 2.6.0).

## Tests on the device (HIL)

- Listening: `test_lldp.py` reads the status screen and checks that a switch
  is shown within 35 s of boot (the EX2200 sends every 30 s). Opt-in via
  `RFUPS_LLDP_SWITCH=1`, since another network may have no LLDP.
- Sending: needs the switch's view. The EX2200-C's serial console is on a
  second USB serial port (COM45 on the user's PC, 9600 baud, the Junos default).
  Opt-in `RFUPS_LLDP_SWITCH_CONSOLE=<port>`: log in with the credentials in a
  local, git-ignored file (`secret/switch.md`, lines `username: ...` and
  `password: ...`; path overridable with `RFUPS_LLDP_SWITCH_CREDENTIALS`), run
  `show lldp neighbors` in the CLI, and check the device's hostname is listed;
  then log out. The test never prints or logs the credentials, and leaves the
  switch configuration untouched (operational commands only).
- The captured EX2200-C frame for the native tests is taken from the device
  itself (a hidden menu command that prints the last received LLDPDU in hex),
  in the same way as other hidden test commands (`stall0`/`stall1`).

## Left to the builder

- Exact struct and function names, the field length caps (suggested: 64 bytes
  per text field), the send buffer size (it fits well under 300 bytes; static).
- Exactly where the web row and the menu line go within their sections.
