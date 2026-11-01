# SNMP MIB

The device runs an SNMP agent on UDP port 161 and sends traps. Every object is read-only.

- **Transport:** UDP port 161 over IPv4 and IPv6 (see the README, IPv6).
- **Versions:** SNMPv1 and SNMPv2c. Each request is answered in the version it was sent in. SNMPv3 requests get no response.
- **Community:** from the settings, default `public`, changed with menu option 4 (active immediately). A request with the wrong community gets no response; it is counted on the status screen.
- **sysName, sysContact, sysLocation:** set with menu option 4 (sysName also on the web page); read-only over SNMP. sysName is the device hostname, also used for DHCP and mDNS (`<sysName>.local`).
- **Interfaces:** the standard IF-MIB (`ifTable`, `ifXTable`) for the one Ethernet port, so generic managers such as LibreNMS graph its traffic.
- **Operations:** GetRequest, GetNextRequest (and so `snmpwalk`), and in v2c GetBulkRequest (`snmpbulkwalk`, `snmpbulkget`). SetRequest is rejected (v1 `noSuchName`, v2c `notWritable`/`noCreation`).
- **Enterprise number:** 63050 (`1.3.6.1.4.1.63050`), assigned to this project.
- **MIB file:** [mibs/RF-UPS-MONITOR-MIB.txt](../mibs/RF-UPS-MONITOR-MIB.txt) (SMIv2).

## Objects

| OID | Name | Type | Value |
|---|---|---|---|
| `1.3.6.1.2.1.1.1.0` | sysDescr | OCTET STRING | `RF-UPS-Monitor 2.3.0 mains power monitor (RP2040, W5500)` (with the firmware version) |
| `1.3.6.1.2.1.1.2.0` | sysObjectID | OBJECT IDENTIFIER | `1.3.6.1.4.1.63050` |
| `1.3.6.1.2.1.1.3.0` | sysUpTime | TimeTicks | Hundredths of a second since boot |
| `1.3.6.1.2.1.1.4.0` | sysContact | OCTET STRING | From the settings (menu option 4), default empty |
| `1.3.6.1.2.1.1.5.0` | sysName | OCTET STRING | The hostname (menu option 4 or web page), default `rf-ups-monitor` |
| `1.3.6.1.2.1.1.6.0` | sysLocation | OCTET STRING | From the settings (menu option 4), default empty |
| `1.3.6.1.2.1.1.7.0` | sysServices | INTEGER | 72 (end-to-end + applications) |
| `1.3.6.1.4.1.63050.1.0` | mainsPowerStatus | INTEGER | `1` = present, `0` = lost |
| `1.3.6.1.4.1.63050.2.0` | mainsPowerChanges | Counter32 | Mains changes since boot (2 per outage) |
| `1.3.6.1.4.1.63050.3.0` | mainsPowerLastChange | TimeTicks | sysUpTime at the last change, 0 if none |
| `1.3.6.1.4.1.63050.4.0` | cpuTemperature | Integer32 | RP2040 die temperature in 0.1 °C (`392` = 39.2 °C), average of 8 readings, updated every 2 s |
| `1.3.6.1.4.1.63050.5.0` | ambientTemperature | Integer32 | TMP1075 temperature in 0.1 °C (`271` = 27.1 °C), read every 2 s; absent without the sensor |
| `1.3.6.1.4.1.63050.6.0` | mainsPowerAlarm | INTEGER | `1` = mains lost for 60 s or more, `0` = none |

`cpuTemperature` comes from the RP2040's internal sensor. It measures the chip, which runs a few degrees above the air around it, and the sensor is only accurate to a few degrees: use it to spot an overheating chip, not as a room thermometer. It was named `boardTemperature` before; the OID and value are unchanged, only the name in the MIB file.

`ambientTemperature` comes from the TMP1075 on the carrier board (I2C0, GP4/GP5, address 0x48), accurate to ±1 °C. It reads the air at the board, several degrees below `cpuTemperature` (27 °C against 34 °C on the bench). Without the carrier board, or after three failed reads in a row (6 s), the object has no instance: v2c Get returns noSuchInstance, v1 Get noSuchName, and walks skip it. It comes back on the next good read.

`mainsPowerStatus` is debounced: a change must last 5 ms to count and is reported within 10 ms of the input changing. On the carrier board the detector holds the input 30-50 ms after mains is lost, so a loss shows within 80 ms of mains going off. To detect an outage between two polls, compare `mainsPowerChanges` with the previous poll; `mainsPowerLastChange` tells you when it happened.

`mainsPowerAlarm` becomes `1` once mains has been lost for 60 s without a break, counted from the debounced loss (from start-up if the device starts without mains), and goes back to `0` when mains returns. A loss that is restored, even briefly, starts a new 60 s. Alert on this object rather than on `mainsPowerStatus` to ignore short outages; when it becomes `1` the device sends one `mainsPowerLostTooLong` trap (below). The 60 s is fixed.

## Interfaces (IF-MIB)

The device has one interface, index 1: the W5500 Ethernet port. `ifNumber.0` (`1.3.6.1.2.1.2.1.0`) is 1.

**ifTable** `1.3.6.1.2.1.2.2.1.<column>.1`

| Column | Name | Type | Value |
|---|---|---|---|
| 1 | ifIndex | INTEGER | 1 |
| 2 | ifDescr | OCTET STRING | `W5500 10/100 Ethernet` |
| 3 | ifType | INTEGER | 6 (ethernetCsmacd) |
| 4 | ifMtu | INTEGER | 1500 |
| 5 | ifSpeed | Gauge32 | 10000000 or 100000000 from the PHY, 0 when the link is down |
| 6 | ifPhysAddress | OCTET STRING | The MAC address (6 bytes) |
| 7 | ifAdminStatus | INTEGER | 1 (up) |
| 8 | ifOperStatus | INTEGER | 1 (up) or 2 (down), from the PHY link bit |
| 9 | ifLastChange | TimeTicks | sysUpTime when the link last changed |
| 10, 16 | ifInOctets, ifOutOctets | Counter32 | Low 32 bits of the 64-bit counters |
| 11, 17 | ifInUcastPkts, ifOutUcastPkts | Counter32 | Unicast frames |
| 12, 18 | ifInNUcastPkts, ifOutNUcastPkts | Counter32 | Multicast + broadcast frames |
| 13 | ifInDiscards | Counter32 | Frames the driver read from the W5500 but dropped because lwIP had no free buffer |
| 14 | ifInErrors | Counter32 | Frames shorter than the 14-byte Ethernet header or longer than 1514 bytes |
| 15 | ifInUnknownProtos | Counter32 | Frames with an EtherType other than IPv4, ARP, IPv6 or LLDP, e.g. spanning tree, VLAN-tagged |
| 19 | ifOutDiscards | Counter32 | 0 (lwIP drops before the driver are not visible) |
| 20 | ifOutErrors | Counter32 | Frames the driver failed to send |
| 21 | ifOutQLen | Gauge32 | 0 |
| 22 | ifSpecific | OBJECT IDENTIFIER | `0.0` |

**ifXTable** `1.3.6.1.2.1.31.1.1.1.<column>.1`

| Column | Name | Type | Value |
|---|---|---|---|
| 1 | ifName | OCTET STRING | `eth0` |
| 2-5 | ifInMulticastPkts, ifInBroadcastPkts, ifOutMulticastPkts, ifOutBroadcastPkts | Counter32 | |
| 6-13 | ifHCInOctets, ifHCInUcastPkts, ifHCInMulticastPkts, ifHCInBroadcastPkts, ifHCOutOctets, ifHCOutUcastPkts, ifHCOutMulticastPkts, ifHCOutBroadcastPkts | Counter64 | **v2c only** |
| 14 | ifLinkUpDownTrapEnable | INTEGER | 2 (disabled) |
| 15 | ifHighSpeed | Gauge32 | 10 or 100 (Mbit/s), 0 when down |
| 16 | ifPromiscuousMode | INTEGER | 2 (false) |
| 17 | ifConnectorPresent | INTEGER | 1 (true) |
| 18 | ifAlias | OCTET STRING | empty |
| 19 | ifCounterDiscontinuityTime | TimeTicks | 0 (counters start at boot) |

The W5500 runs in MAC-raw mode for lwIP and has no traffic counters of its own, so the firmware counts every frame passed between the W5500 and lwIP: octets are whole Ethernet frames without the FCS, and frames are sorted into unicast, multicast and broadcast by their destination MAC. Only frames lwIP can use count as packets; the others count in ifInErrors or ifInUnknownProtos (all received octets count in ifInOctets).

What the counters cannot see: the W5500 itself silently drops frames with a bad checksum and frames that arrive while its 16 KB receive buffer is full, and has no counters for either. ifInDiscards therefore only counts drops in the firmware. ifInUnknownProtos is normally non-zero and rising on a LAN with spanning tree or other protocols the device does not use; that is not a fault. LLDP frames from the switch count as multicast (ifInMulticastPkts), since the device reads them.

The counters are 64-bit internally and start at 0 at boot. Link state and speed are read from the W5500 PHY register (`PHYCFGR`) every 500 ms; a link change is logged on the serial port.

SNMPv1 has no Counter64 type, so a v1 request does not see the eight `ifHC*` columns at all (RFC 3584): a v1 Get of one is `noSuchName` and a v1 walk skips them. A v1 walk returns 45 objects, a v2c walk 53.

## v1 and v2c differences

The same request gets a different answer depending on its version:

| Situation | v1 | v2c |
|---|---|---|
| Counter64 objects (`ifHC*`) | Do not exist (`noSuchName`, skipped by GetNext) | Normal objects |
| Get of an OID that does not exist | Whole request fails: `noSuchName`, error-index = position | Request succeeds; that varbind is `noSuchObject` (no such object) or `noSuchInstance` (object exists, e.g. `sysDescr.5` or `sysDescr` without `.0`) |
| GetNext past the last object | `noSuchName` | `endOfMibView` for that varbind |
| Set | `noSuchName` | `notWritable` (existing object) or `noCreation` |
| GetBulk | not available | Non-repeaters once, then up to max-repetitions rows; stops early when every repeater reached `endOfMibView`; returns at most 16 varbinds (fewer if they do not fit) instead of `tooBig` |
| Response too big | `tooBig` with the request varbinds | `tooBig` with an empty varbind list |

With net-snmp, v2c shows these as `No Such Object available on this agent at this OID`, `No Such Instance currently exists at this OID` and `No more variables left in this MIB View`.

## Traps

Traps are sent to the trap destination set in menu option 4 (a fixed address, or `dhcp`: see [Trap destination from DHCP](#trap-destination-from-dhcp)) (UDP 162, community = the configured community), in the trap version chosen there: **v2c (default)** or v1. With no destination set, no traps are sent. The fixed address can be IPv4 or IPv6 (global or ULA); an IPv6 destination gets v2c traps only, since the v1 Trap-PDU's agent-addr is an IPv4 field.

| Trap | v1: generic / specific | v2c: snmpTrapOID.0 | Variable bindings | When |
|---|---|---|---|---|
| coldStart | 0 / 0 | `1.3.6.1.6.3.1.1.5.1` | none | Once per boot, when the network first comes up |
| authenticationFailure | 4 / 0 | `1.3.6.1.6.3.1.1.5.5` | none | Request with a wrong community; at most one per 10 s |
| mainsPowerLost | 6 (enterpriseSpecific) / 1 | `1.3.6.1.4.1.63050.0.1` | mainsPowerStatus, mainsPowerChanges, mainsPowerLastChange | Mains lost |
| mainsPowerRestored | 6 (enterpriseSpecific) / 2 | `1.3.6.1.4.1.63050.0.2` | same | Mains restored |
| mainsPowerLostTooLong | 6 (enterpriseSpecific) / 3 | `1.3.6.1.4.1.63050.0.3` | same, then mainsPowerAlarm | Mains lost for 60 s (mainsPowerAlarm became 1); once per outage |

- **v1** traps carry enterprise `1.3.6.1.4.1.63050` and the device's IP as agent-addr.
- **v2c** traps start with `sysUpTime.0` and `snmpTrapOID.0`, as RFC 3416 requires, then the variable bindings above, and end with `snmpTrapAddress.0` (`1.3.6.1.6.3.18.1.3.0`, IpAddress, RFC 3584): the device's IPv4 address, as the v1 agent-addr. A manager behind NAT, such as LibreNMS in Docker Desktop, sees another sender address and can find the device by this one. It is left out while the device has no IPv4 address. Each trap has its own request-id.
- The time (v1 time-stamp, v2c sysUpTime.0) of a mains trap is the moment of the change, equal to mainsPowerLastChange, not when the trap was sent.
- In the MIB file the mains traps are the notifications `mainsPowerLost` (`63050.0.1`), `mainsPowerRestored` (`63050.0.2`) and `mainsPowerLostTooLong` (`63050.0.3`); RFC 3584 maps them to the v1 traps above.
- `mainsPowerLostTooLong` is held until it can be sent: if there is no trap destination when the alarm is raised (network not up yet, no destination set, DHCP destination not received yet), it goes out once as soon as there is one, unless mains returns first. There is no separate trap when the alarm clears; `mainsPowerRestored` marks that.

Traps are not acknowledged (v2c InformRequest is not used), so a manager should also poll `mainsPowerChanges`: if it changed between two polls, an outage happened even if its traps were lost.

To receive traps on Linux, run `snmptrapd -f -Lo` (package `snmptrapd`). On Windows, allow inbound UDP 162 in the firewall for the receiving program.

### Trap destination from DHCP

Instead of a fixed address, the trap destination can come from the DHCP server: enter `dhcp` as the trap destination in menu option 4. The device then asks for DHCP option **224** (a site-specific option carrying one IPv4 address) in every DHCP request, and sends traps to the address in the latest DHCPACK for its current lease. If the ACK has no option 224, or the device uses a static IP, no traps are sent. Choosing `dhcp` renews the lease at once, so the address arrives within seconds; the status screen shows it:

```
Trap destination: "10.10.6.65" (12 sent, 0 failed)
Trap destination source: DHCP option 224 (asked in 3 DHCP messages; last ACK 40 s ago: 10.10.6.65)
```

Configure option 224 on the DHCP server. The device asks for it, so the server does not have to be told to send it unasked:

```jsonc
// Kea (kea-dhcp4.conf, in "Dhcp4"); tested with Kea 2.2.0
"option-def": [ { "name": "rfups-trap-target", "code": 224, "type": "ipv4-address", "space": "dhcp4" } ],
"subnet4": [ { ..., "option-data": [ { "name": "rfups-trap-target", "data": "10.10.6.65" } ] } ]
```

```
# ISC dhcpd (dhcpd.conf)
option rfups-trap-target code 224 = ip-address;
option rfups-trap-target 10.10.6.65;

# dnsmasq
dhcp-option=224,10.10.6.65
```

How it works: lwIP is compiled with the Arduino-Pico core's `lwipopts.h`, which does not ask for or keep extra DHCP options (lwIP's `DHCP_ADD_EXTRA_REQUEST_OPTIONS` and DHCP hooks are not set), so the firmware edits the device's own outgoing DHCP DISCOVER/REQUEST at the driver: it appends 224 to the Parameter Request List (option 55), using one of the padding bytes lwIP always leaves after the END option, and fixes the UDP checksum. Option 224 is taken only from a DHCPACK that the device's DHCP client (lwIP) actually accepted: lwIP must be waiting for an ACK, the frame must carry lwIP's current transaction id and the server id of the server lwIP talks to, and lwIP must become bound while processing that frame. A forged "ACK" sent by another host on the LAN is ignored (and counted on the status screen: `DHCP: N ACK-like frames ignored`). With the trap destination fixed or off, DHCP messages are not changed.

Remaining risk: DHCP has no authentication. A host that sees the device's DHCP request (they are broadcast) and answers faster than the real server can hand out the address, gateway and DNS, and so also option 224. Traps carry the SNMP community in clear text, so such a host would learn it. Use `dhcp` only on a network where you trust the DHCP server and its neighbours; otherwise set a fixed trap destination.

## Rate limiting and access log

Each source address (IPv4 or the full IPv6 address) may send 50 requests per second on average, with bursts of up to 100. Requests beyond that are dropped without a response and counted on the status screen ("rate limited"). Up to 8 sources are tracked at once; when more appear, the one seen least recently is forgotten. A flood from one source does not affect others.

On top of that, all untrusted sources together may send at most 100 requests per second (bursts of 200). Source addresses are easy to fake with UDP, and a new address starts with a full bucket, so without this global limit an attacker rotating through many addresses could bypass the per-source limit. A request refused for its own source does not count against the global limit.

A source becomes trusted once it sends a request with the right community. Trusted sources (your SNMP managers) use a separate shared limit of 200 requests per second (bursts of 400) instead of the untrusted one, so a flood from addresses that do not know the community cannot starve them. They are also kept in the 8-entry table in preference to untrusted sources, so such a flood cannot push them out and reset their state.

Trust belongs to the exact address that sent the right community. IPv6 sources are not grouped by /64: all hosts on a LAN share one /64, so grouping would hand a manager's trust (and its budget) to every neighbour. A host rotating through many IPv6 addresses gets nothing more than one faking IPv4 addresses: each new address is untrusted and draws from the shared untrusted limit.

Trust is deliberately bounded, because the community is only a weak secret: the default `public` is well known, and SNMPv1 sends it in clear text.
- At most 4 sources are trusted at once.
- Trust expires after 10 minutes without requests.
- Changing the community (menu option 4) revokes all trust immediately.
- All trusted sources together stay within 200/s, so knowing the community does not give unlimited throughput.

Anyone who knows the community is treated like a manager; that is a limit of SNMPv1. **Change the community from `public`** to keep this protection meaningful. The limiter keeps working after any idle period, including gaps of more than 24.8 days (the point where `millis()` differences turn negative when read as signed values).

Rejected requests are recorded in the access log (menu option 5): wrong community, SNMPv2c/v3, malformed or oversized packets, rate-limited requests and SetRequests, with the source address. The newest 16 entries are kept; repeats from the same source are folded into one entry with a count.

## Examples

From Linux or WSL with net-snmp (`apt install snmp`):

```bash
# Everything (v1, v2c, v2c with GetBulk)
snmpwalk -v1 -c public 10.10.6.111
snmpwalk -v2c -c public 10.10.6.111
snmpbulkwalk -v2c -c public 10.10.6.111

# Mains state
snmpget -v1 -c public 10.10.6.111 1.3.6.1.4.1.63050.1.0
# .1.3.6.1.4.1.63050.1.0 = INTEGER: 1

# Mains state, change count and last change in one request
snmpget -v1 -c public 10.10.6.111 1.3.6.1.4.1.63050.1.0 1.3.6.1.4.1.63050.2.0 1.3.6.1.4.1.63050.3.0
```

With the MIB file, the tools show names instead of numbers:

```bash
snmpwalk -v1 -c public -M +./mibs -m +RF-UPS-MONITOR-MIB 10.10.6.111 1.3.6.1.4.1.63050
# RF-UPS-MONITOR-MIB::mainsPowerStatus.0 = INTEGER: present(1)
# RF-UPS-MONITOR-MIB::mainsPowerChanges.0 = Counter32: 0
# RF-UPS-MONITOR-MIB::mainsPowerLastChange.0 = Timeticks: (0) 0:00:00.00
```

This needs the standard IETF MIBs, which Debian/Ubuntu ship separately: `sudo apt install snmp-mibs-downloader`.

## Errors

| Situation | Response |
|---|---|
| GetRequest for an OID that is not an object (including non-leaf OIDs such as `1.3.6.1.2.1.1.1`) | `noSuchName`, error-index = position of the first unknown OID |
| GetNextRequest past the last object | v1 `noSuchName` (net-snmp prints "End of MIB" during a walk); v2c `endOfMibView` |
| SetRequest | v1 `noSuchName`; v2c `notWritable` / `noCreation`; for the first varbind |
| Response would exceed 1472 bytes | `tooBig` (GetBulk is truncated instead) |
| Wrong community, SNMPv3, malformed, trap/inform/response PDU, more than 16 varbinds | No response (logged in the access log) |
| Source over the rate limit | No response (logged) |

## Limits

- Up to 16 variable bindings per request and at most 16 in a GetBulk response. A larger GetBulk reply would let a small request with a faked source address make the device send a much larger packet to a victim (reflection); a bulk walk simply takes more requests (4 for the 53 objects). A GetBulk request also does not make its source trusted for the rate limiter.
- Requests up to 1472 bytes (one Ethernet frame); larger packets are dropped. Replies over IPv6 are at most 1452 bytes (the IPv6 header is 20 bytes longer; the device does not fragment); GetBulk replies are shortened to fit.
- The network stack queues up to 5 UDP packets. Requests sent in a burst beyond that are dropped; SNMP managers retry, and normal polling or `snmpwalk` sends one request at a time.
- If the device has not talked to a manager for a few minutes, it first has to look up the manager's MAC address (ARP). lwIP holds one reply during that lookup, so only one request of a burst arriving at that moment is answered.
- Typical response time is about 4 ms on the local network (requirement: 100 ms).

## Implementation

| Layer | Code | Tests |
|---|---|---|
| BER encoding/decoding, OIDs | `lib/snmp_ber` | `test/native/test_snmp_ber` |
| Message parsing and responses | `lib/snmp_agent/snmp_message` | `test/native/test_snmp_message` |
| MIB table, Get/GetNext/Set handling | `lib/snmp_agent/snmp_mib`, `snmp_agent` | `test/native/test_snmp_agent` |
| Frame counting, PHY decoding | `lib/if_stats` | `test/native/test_if_stats` |
| Counters and PHY on the device | `src/net_stats.cpp` | `test/hil/test_snmp.py` |
| CPU temperature | `src/cpu_temp.cpp` | `test/hil/test_snmp.py` |
| Ambient temperature (TMP1075) | `lib/tmp1075`, `src/ambient_temp.cpp` | `test/native/test_tmp1075`, `test/native/test_snmp_agent`, `test/hil/test_snmp.py` |
| Trap encoding (v1 and v2c), throttle | `lib/snmp_agent/snmp_trap` | `test/native/test_snmp_trap` (byte-exact vs net-snmp `snmptrap -v1` and `-v2c`) |
| Rate limiter, access log | `lib/access_control` | `test/native/test_access_control` |
| Traps on the device | `src/snmp_service.cpp` | `test/hil/test_snmp_traps.py` |
| Against real net-snmp, without hardware | `test/host/host_agent.cpp` | `scripts/host_agent_check.sh` |
| UDP port 161 on the device | `src/snmp_service.cpp` | `test/hil/test_snmp.py` |

To add an object: add a getter and an entry in `lib/snmp_agent/src/snmp_mib.cpp` (keeping the table sorted), extend `test_mib_contains_expected_objects` and the walk checks, add it to the MIB file and to this table.
