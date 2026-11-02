# SNMP MIB

The device runs an SNMP agent on UDP port 161 and sends traps. Every object is read-only.

- **Transport:** UDP port 161 over IPv4 and IPv6 (see the README, IPv6).
- **Versions:** SNMPv1 and SNMPv2c. Each request is answered in the version it was sent in. SNMPv3 requests get no response.
- **Community:** from the settings, default `public`, changed with menu option 4 (active immediately). A request with the wrong community gets no response; it is counted on the status screen.
- **sysName, sysContact, sysLocation:** set with menu option 4 or on the web page (Device name); read-only over SNMP. sysName is the device hostname, also used for DHCP and mDNS (`<sysName>.local`).
- **Interfaces:** the standard IF-MIB (`ifTable`, `ifXTable`) for the one Ethernet port, so generic managers such as LibreNMS graph its traffic.
- **Operations:** GetRequest, GetNextRequest (and so `snmpwalk`), and in v2c GetBulkRequest (`snmpbulkwalk`, `snmpbulkget`). SetRequest is rejected (v1 `noSuchName`, v2c `notWritable`/`noCreation`).
- **Enterprise number:** 63050 (`1.3.6.1.4.1.63050`), assigned to this project.
- **MIB file:** [mibs/RF-POWER-MONITOR-MIB.txt](../mibs/RF-POWER-MONITOR-MIB.txt) (SMIv2).

## Objects

| OID | Name | Type | Value |
|---|---|---|---|
| `1.3.6.1.2.1.1.1.0` | sysDescr | OCTET STRING | `RF-Power-Monitor 3.1.0 mains power monitor (RP2040, W5500)` (with the firmware version) |
| `1.3.6.1.2.1.1.2.0` | sysObjectID | OBJECT IDENTIFIER | `1.3.6.1.4.1.63050` |
| `1.3.6.1.2.1.1.3.0` | sysUpTime | TimeTicks | Hundredths of a second since boot |
| `1.3.6.1.2.1.1.4.0` | sysContact | OCTET STRING | From the settings (menu option 4 or web page), default empty |
| `1.3.6.1.2.1.1.5.0` | sysName | OCTET STRING | The hostname (menu option 4 or web page), default `rf-power-monitor` |
| `1.3.6.1.2.1.1.6.0` | sysLocation | OCTET STRING | From the settings (menu option 4 or web page), default empty |
| `1.3.6.1.2.1.1.7.0` | sysServices | INTEGER | 72 (end-to-end + applications) |
| `1.3.6.1.2.1.1.8.0` | sysORLastChange | TimeTicks | 0 (the table below never changes) |
| `1.3.6.1.2.1.1.9.1.{2,3,4}.n` | sysORTable: sysORID, sysORDescr, sysORUpTime | OID, OCTET STRING, TimeTicks 0 | One row per MIB the agent implements, see below |
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

## SNMPv2-MIB: sysORTable, snmp group, snmpSetSerialNo

The agent implements the whole of SNMPv2-MIB (RFC 3418).

**sysORTable** (`1.3.6.1.2.1.1.9.1.<column>.<n>`) lists the MIB modules the agent implements, n = 1-9:

| n | sysORID | sysORDescr |
|---|---|---|
| 1 | `1.3.6.1.6.3.1` (snmpMIB) | SNMPv2-MIB |
| 2 | `1.3.6.1.2.1.31` (ifMIB) | IF-MIB |
| 3 | `1.3.6.1.2.1.48` (ipMIB) | IP-MIB |
| 4 | `1.3.6.1.2.1.50` (udpMIB) | UDP-MIB |
| 5 | `1.3.6.1.2.1.49` (tcpMIB) | TCP-MIB |
| 6 | `1.3.6.1.2.1.25.7.1` (hostResourcesMibModule) | HOST-RESOURCES-MIB |
| 7 | `1.3.6.1.2.1.47` (entityMIB) | ENTITY-MIB |
| 8 | `1.0.8802.1.1.2` (lldpMIB) | LLDP-MIB |
| 9 | `1.3.6.1.4.1.63050` | RF-POWER-MONITOR-MIB |

sysORUpTime is 0 for every row: all are there from boot.

**snmp group** (`1.3.6.1.2.1.11.<n>.0`, Counter32 unless noted), the agent's own counters since boot. They are also on the status screen.

| n | Object | Value |
|---|---|---|
| 1 | snmpInPkts | Every packet the agent read, including the request being answered |
| 2 | snmpOutPkts | Responses plus traps sent |
| 3 | snmpInBadVersions | Neither v1 nor v2c (SNMPv3 included) |
| 4 | snmpInBadCommunityNames | Wrong community |
| 5 | snmpInBadCommunityUses | 0: the one community may do everything there is (read) |
| 6 | snmpInASNParseErrs | Malformed packets |
| 8-12 | snmpInTooBigs, snmpInNoSuchNames, snmpInBadValues, snmpInReadOnlys, snmpInGenErrs | 0: these count error responses received, which the agent never accepts |
| 13 | snmpInTotalReqVars | Varbinds answered with a value by Get, GetNext and GetBulk (exceptions and error responses not counted) |
| 14 | snmpInTotalSetVars | 0: nothing is writable |
| 15, 16, 17 | snmpInGetRequests, snmpInGetNexts, snmpInSetRequests | Requests with the right community (GetBulk has no counter here) |
| 18, 19 | snmpInGetResponses, snmpInTraps | Response and trap PDUs received (dropped unanswered) |
| 20, 21 | snmpOutTooBigs, snmpOutNoSuchNames | Error responses sent |
| 22, 24 | snmpOutBadValues, snmpOutGenErrs | 0 |
| 25-27 | snmpOutGetRequests, snmpOutGetNexts, snmpOutSetRequests | 0: the device is not a manager |
| 28 | snmpOutGetResponses | Responses sent |
| 29 | snmpOutTraps | Traps sent |
| 30 | snmpEnableAuthenTraps | INTEGER 1 (enabled): the device sends authenticationFailure (read-only) |
| 31 | snmpSilentDrops | Requests dropped because not even an empty response fit in 1472 bytes |
| 32 | snmpProxyDrops | 0: no proxy |

Requests the rate limiter drops never reach the agent, so no counter here sees them; the status screen counts them (`rate limited`).

**snmpSetSerialNo** (`1.3.6.1.6.3.1.1.6.1.0`, INTEGER) is 0 and read-only: it coordinates Sets between managers, and the device accepts none. It sorts last, so a walk from the root ends with it.

## IP, ICMP, TCP and UDP statistics (MIB-II)

The scalars of the MIB-II ip (`1.3.6.1.2.1.4`), icmp (`.5`), tcp (`.6`) and udp (`.7`) groups (IP-MIB RFC 4293, TCP-MIB RFC 4022, UDP-MIB RFC 4113) come from lwIP's own MIB-II counters (`MIB2_STATS`). The firmware switches them on in `include/lwipopts.h`: the Arduino-Pico core turns lwIP's statistics off, and this file loads the core's options first and then changes only those. They cost 196 bytes of RAM. LibreNMS draws its IP, ICMP, TCP and UDP graphs from them.

| Group | Objects | Notes |
|---|---|---|
| ip | `.4.1.0`-`.4.19.0`, `.4.23.0` | IPv4 only. ipForwarding 2 (not forwarding), ipDefaultTTL 255, ipReasmTimeout 15 (seconds). ipForwDatagrams and ipRoutingDiscards stay 0: the device does not route. |
| icmp | `.5.1.0`-`.5.26.0` | IPv4 only. In: every type. Out: messages, errors, destination unreachable, time exceeded, echo requests, echo replies; lwIP never sends the other types, so their counters are 0. |
| tcp | `.6.1.0`-`.6.12.0`, `.6.14.0`, `.6.15.0` | IPv4 and IPv6. tcpRtoAlgorithm 4 (vanj); tcpRtoMin 1000 and tcpRtoMax 60000 ms are lwIP's nominal values (lwIP's own SNMP agent reports the same); tcpMaxConn 5; tcpCurrEstab (Gauge32) counts connections in ESTABLISHED or CLOSE-WAIT. tcpConnTable (`.6.13`) is not served. |
| udp | `.7.1.0`-`.7.4.0` | IPv4 and IPv6. udpInDatagrams counts datagrams delivered to an open port, udpNoPorts those to a closed one. |

The counters start at 0 at boot. Counters lwIP keeps but never increments on this device (for example ipReasmFails without fragmented traffic) read 0, which is their true value.

## IP addresses (IP-MIB)

The device's addresses are listed twice, as IP-MIB defines: in the older `ipAddrTable` (IPv4 only, which LibreNMS reads for IPv4) and in `ipAddressTable` (IPv4 and IPv6). The rows follow the addresses the device has at the moment of the request: they appear and go as DHCP, SLAAC, DHCPv6 or the static settings change them, and a walk that was at an address that has just gone simply continues with the next one.

**ipAddrTable** `1.3.6.1.2.1.4.20.1.<column>.<a.b.c.d>`, one row while the device has an IPv4 address

| Column | Name | Value |
|---|---|---|
| 1 | ipAdEntAddr | The address (IpAddress) |
| 2 | ipAdEntIfIndex | 1 |
| 3 | ipAdEntNetMask | The subnet mask |
| 4 | ipAdEntBcastAddr | 1 (broadcast with the host part all ones) |
| 5 | ipAdEntReasmMaxSize | 65535 |

**ipAddressTable** `1.3.6.1.2.1.4.34.1.<column>.<type>.<length>.<address bytes>`, type 1 = ipv4 (length 4), 2 = ipv6 (length 16); one row per valid address: the IPv4 address and every IPv6 address that is preferred or deprecated (not while duplicate detection runs)

| Column | Name | Value |
|---|---|---|
| 3 | ipAddressIfIndex | 1 |
| 4 | ipAddressType | 1 (unicast) |
| 5 | ipAddressPrefix | Pointer to the prefix row, `1.3.6.1.2.1.4.32.1.5.1.<type>.<length>.<network bytes>.<prefix length>`. ipAddressPrefixTable itself is not served; managers read the prefix length from the pointer. IPv6 addresses are /64 (lwIP keeps no prefix length; link-local, SLAAC and the static address are /64). |
| 6 | ipAddressOrigin | IPv4: 4 (dhcp) or 2 (manual, static IP). IPv6: 5 (linklayer: link-local and SLAAC, both from the MAC), 4 (dhcp: DHCPv6) or 2 (manual: the static address) |
| 7 | ipAddressStatus | 1 (preferred) or 2 (deprecated) |
| 8, 9 | ipAddressCreated, ipAddressLastChanged | TimeTicks 0 (not recorded) |
| 10 | ipAddressRowStatus | 1 (active) |
| 11 | ipAddressStorageType | 2 (volatile) |

An IPv6 row's index is 28 arcs long, so the whole OID fits the agent's 32-arc limit.

## Host resources (HOST-RESOURCES-MIB) and inventory (ENTITY-MIB)

LibreNMS and other managers read CPU load, memory and storage from the standard HOST-RESOURCES-MIB (RFC 2790), and the inventory from ENTITY-MIB (RFC 6933), so they show these without device-specific setup.

**hrSystem and memory** `1.3.6.1.2.1.25`

| OID | Name | Type | Value |
|---|---|---|---|
| `.1.1.0` | hrSystemUptime | TimeTicks | Same as sysUpTime |
| `.1.5.0` | hrSystemNumUsers | Gauge32 | 0 |
| `.1.6.0` | hrSystemProcesses | Gauge32 | 2: the loops of core 0 and core 1 |
| `.1.7.0` | hrSystemMaxProcesses | INTEGER | 2 |
| `.2.2.0` | hrMemorySize | INTEGER (KB) | 256 |

hrSystemDate is not served: the device has no clock.

**hrStorageTable** `1.3.6.1.2.1.25.2.3.1.<column>.<index>`, index 1 = RAM, 2 = LittleFS

| Column | Name | RAM (1) | LittleFS (2) |
|---|---|---|---|
| 1 | hrStorageIndex | 1 | 2 |
| 2 | hrStorageType | hrStorageRam (`.25.2.1.2`) | hrStorageFlashMemory (`.25.2.1.9`) |
| 3 | hrStorageDescr | `RAM` | `LittleFS (settings)` |
| 4 | hrStorageAllocationUnits | 1024 | 1024 |
| 5 | hrStorageSize | 256 (the RP2040's 256 KB main SRAM) | 256 (the 0.25 MB settings partition) |
| 6 | hrStorageUsed | KB in use, rounded up: static data, both stacks and the heap in use (256 KB minus free heap; the status screen shows `RAM used`) | KB of LittleFS blocks in use, read every 10 s |
| 7 | hrStorageAllocationFailures | Counter32 0 | Counter32 0 |

The LittleFS row disappears if the filesystem cannot be read.

**hrDeviceTable** `1.3.6.1.2.1.25.3.2.1.<column>.<index>` and **hrProcessorTable** `1.3.6.1.2.1.25.3.3.1.<column>.<index>`, index 768 = core 0, 769 = core 1

| Column | Name | Value |
|---|---|---|
| hrDevice 1 | hrDeviceIndex | 768, 769 |
| hrDevice 2 | hrDeviceType | hrDeviceProcessor (`.25.3.1.3`) |
| hrDevice 3 | hrDeviceDescr | `RP2040 core 0 (network, SNMP)`, `RP2040 core 1 (mains input)` |
| hrDevice 4 | hrDeviceID | `0.0` (unknown) |
| hrDevice 5 | hrDeviceStatus | 2 (running) |
| hrDevice 6 | hrDeviceErrors | Counter32 0 |
| hrProcessor 1 | hrProcessorFrwID | `0.0` (unknown) |
| hrProcessor 2 | hrProcessorLoad | INTEGER 0-100: percent of the last minute the core was not idle |

How the load is measured (`lib/cpu_load`):
- **Core 1** sleeps 1 ms after each round of sampling the mains input and the reset button; its load is the time it spends outside that sleep (about 1 %).
- **Core 0** never sleeps: it polls the network, SNMP, the web page and the menu in a loop. Work makes the loop's passes longer, so fewer fit in a second. Its load is `1 - passes in a second / most passes per second in the last minute`. An idle device does about 14,000 passes per second and reads 0 %; a minute of SNMP walks and web page loads at full speed read 87 % in the HIL test. If the device were busy for more than a minute without one idle second, the reference would drop and the load read too low.

Both are also on the status screen (`CPU load: core 0 0 %, core 1 1 % (last minute)`).

**entPhysicalTable** `1.3.6.1.2.1.47.1.1.1.1.<column>.1`: one entity, the device

| Column | Name | Value |
|---|---|---|
| 2 | entPhysicalDescr | `RF-Power-Monitor mains power monitor` |
| 3 | entPhysicalVendorType | `1.3.6.1.4.1.63050` (= sysObjectID) |
| 4 | entPhysicalContainedIn | 0 |
| 5 | entPhysicalClass | 3 (chassis) |
| 6 | entPhysicalParentRelPos | -1 |
| 7 | entPhysicalName | sysName |
| 8 | entPhysicalHardwareRev | `W5500-EVB-Pico-PoE + carrier rev 1.1` (`HARDWARE_REVISION` in `include/version.h`) |
| 9 | entPhysicalFirmwareRev | The firmware version, as in sysDescr |
| 10 | entPhysicalSoftwareRev | empty |
| 11 | entPhysicalSerialNum | The flash chip's unique ID, 16 hex digits (also on the status screen) |
| 12 | entPhysicalMfgName | `Kenneth Foli Jørgensen` (UTF-8) |
| 13 | entPhysicalModelName | `RF-Power-Monitor` |
| 14, 15 | entPhysicalAlias, entPhysicalAssetID | empty |
| 16 | entPhysicalIsFRU | 2 (false) |

`entLastChangeTime` (`1.3.6.1.2.1.47.1.4.1.0`) is 0: the table never changes.

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

SNMPv1 has no Counter64 type, so a v1 request does not see the eight `ifHC*` columns at all (RFC 3584): a v1 Get of one is `noSuchName` and a v1 walk skips them. Without the LLDP-MIB objects and the address rows a v1 walk returns 221 objects, a v2c walk 229 (with them, see below). Each address adds 9 rows to ipAddressTable, the IPv4 address 5 more to ipAddrTable.

## Switch and port (LLDP-MIB)

The device answers part of the standard LLDP-MIB (IEEE 802.1AB-2005 Annex F), so a manager such as LibreNMS can show which switch and port it is plugged into ([LIBRENMS.md](LIBRENMS.md#switch-and-port-lldp)). `1.0.8802...` sorts before `1.3.6.1...`, so a walk from the root (`.1`) starts with these objects; the default walk (mib-2) does not include them.

| OID | Name | Type | Value |
|---|---|---|---|
| 1.0.8802.1.1.2.1.3.7.1.2.1 | lldpLocPortIdSubtype | INTEGER | 3 (macAddress) |
| 1.0.8802.1.1.2.1.3.7.1.3.1 | lldpLocPortId | OCTET STRING | the device's MAC (6 bytes) |
| 1.0.8802.1.1.2.1.3.7.1.4.1 | lldpLocPortDesc | OCTET STRING | `W5500 10/100 Ethernet` (= ifDescr) |
| 1.0.8802.1.1.2.1.4.1.1.4.0.1.1 | lldpRemChassisIdSubtype | INTEGER | as sent by the switch (4 = MAC address) |
| 1.0.8802.1.1.2.1.4.1.1.5.0.1.1 | lldpRemChassisId | OCTET STRING | as sent (MAC: 6 bytes), at most 64 bytes |
| 1.0.8802.1.1.2.1.4.1.1.6.0.1.1 | lldpRemPortIdSubtype | INTEGER | as sent, but 5 (interfaceName) when the switch sent 7 (locally assigned) and a port description |
| 1.0.8802.1.1.2.1.4.1.1.7.0.1.1 | lldpRemPortId | OCTET STRING | as sent, or in that case the port description (Junos: `ge-0/0/9.0` instead of its ifIndex `523`) |
| 1.0.8802.1.1.2.1.4.1.1.8.0.1.1 | lldpRemPortDesc | OCTET STRING | port description, empty if not sent |
| 1.0.8802.1.1.2.1.4.1.1.9.0.1.1 | lldpRemSysName | OCTET STRING | the switch's name |
| 1.0.8802.1.1.2.1.4.1.1.10.0.1.1 | lldpRemSysDesc | OCTET STRING | its system description, at most 128 bytes |
| 1.0.8802.1.1.2.1.4.1.1.11.0.1.1 | lldpRemSysCapSupported | BITS (2 bytes) | as sent, in BITS order (bit 0 = top bit of the first byte; the TLV counts from the bottom bit): bridge + router = `28 00` |
| 1.0.8802.1.1.2.1.4.1.1.12.0.1.1 | lldpRemSysCapEnabled | BITS (2 bytes) | as sent |

There is one remote row, index lldpRemTimeMark 0, lldpRemLocalPortNum 1, lldpRemIndex 1, and only while the device holds a neighbour (the status screen's `Switch:` line); otherwise v1 answers noSuchName and v2c noSuchInstance, and walks skip it. Text from the switch is cut to 64 characters (128 for the description) with non-printable bytes replaced by `?`. Not served: lldpRemManAddrTable (the management address; its index holds the address), lldpLocalSystemData, lldpStatistics and LLDP-V2-MIB. A v1 walk from the root returns 233 objects with a neighbour (224 without), a v2c walk 241 (232), plus the address rows (with one IPv4 and two IPv6 addresses: 5 + 3 x 9 = 32).

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
"option-def": [ { "name": "rfpower-trap-target", "code": 224, "type": "ipv4-address", "space": "dhcp4" } ],
"subnet4": [ { ..., "option-data": [ { "name": "rfpower-trap-target", "data": "10.10.6.65" } ] } ]
```

```
# ISC dhcpd (dhcpd.conf)
option rfpower-trap-target code 224 = ip-address;
option rfpower-trap-target 10.10.6.65;

# dnsmasq
dhcp-option=224,10.10.6.65
```

How it works: lwIP is compiled with the Arduino-Pico core's `lwipopts.h`, which does not ask for or keep extra DHCP options (lwIP's `DHCP_ADD_EXTRA_REQUEST_OPTIONS` and DHCP hooks are not set), so the firmware edits the device's own outgoing DHCP DISCOVER/REQUEST at the driver: it appends 224 to the Parameter Request List (option 55), using one of the padding bytes lwIP always leaves after the END option, and fixes the UDP checksum. Option 224 is taken only from a DHCPACK that the device's DHCP client (lwIP) actually accepted: lwIP must be waiting for an ACK, the frame must carry lwIP's current transaction id and the server id of the server lwIP talks to, and lwIP must become bound while processing that frame. A forged "ACK" sent by another host on the LAN is ignored (and counted on the status screen: `DHCP: N ACK-like frames ignored`). With the trap destination fixed or off, DHCP messages are not changed.

Remaining risk: DHCP has no authentication. A host that sees the device's DHCP request (they are broadcast) and answers faster than the real server can hand out the address, gateway and DNS, and so also option 224. Traps carry the SNMP community in clear text, so such a host would learn it. Use `dhcp` only on a network where you trust the DHCP server and its neighbours; otherwise set a fixed trap destination.

## Rate limiting and access log

Each source address (IPv4 or the full IPv6 address) may send 50 requests per second on average, with bursts of up to 300 (enough for a whole snmpwalk at full speed). Requests beyond that are dropped without a response and counted on the status screen ("rate limited"). Up to 8 sources are tracked at once; when more appear, the one seen least recently is forgotten. A flood from one source does not affect others.

On top of that, all untrusted sources together may send at most 100 requests per second (bursts of 200). Source addresses are easy to fake with UDP, and a new address starts with a full bucket, so without this global limit an attacker rotating through many addresses could bypass the per-source limit. A request refused for its own source does not count against the global limit.

A source becomes trusted once it sends a request with the right community. Trusted sources (your SNMP managers) use a separate shared limit of 200 requests per second (bursts of 400) instead of the untrusted one, so a flood from addresses that do not know the community cannot starve them. They are also kept in the 8-entry table in preference to untrusted sources, so such a flood cannot push them out and reset their state.

Trust belongs to the exact address that sent the right community. IPv6 sources are not grouped by /64: all hosts on a LAN share one /64, so grouping would hand a manager's trust (and its budget) to every neighbour. A host rotating through many IPv6 addresses gets nothing more than one faking IPv4 addresses: each new address is untrusted and draws from the shared untrusted limit.

Trust is deliberately bounded, because the community is only a weak secret: the default `public` is well known, and SNMPv1 sends it in clear text.
- At most 4 sources are trusted at once.
- Trust expires after 10 minutes without requests.
- Changing the community (menu option 4) revokes all trust immediately.
- All trusted sources together stay within 200/s, so knowing the community does not give unlimited throughput.

Anyone who knows the community is treated like a manager; that is a limit of SNMPv1. **Change the community from `public`** to keep this protection meaningful. The limiter keeps working after any idle period, including gaps of more than 24.8 days (the point where `millis()` differences turn negative when read as signed values).

Rejected requests are recorded in the access log (menu option 5): wrong community, SNMPv3 (and unknown versions), malformed or oversized packets, rate-limited requests and SetRequests, plus failed web logins and lockouts, with the source address. The newest 16 entries are kept; a repeat of the newest entry (same source and event) is folded into it with a count.

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
snmpwalk -v1 -c public -M +./mibs -m +RF-POWER-MONITOR-MIB 10.10.6.111 1.3.6.1.4.1.63050
# RF-POWER-MONITOR-MIB::mainsPowerStatus.0 = INTEGER: present(1)
# RF-POWER-MONITOR-MIB::mainsPowerChanges.0 = Counter32: 0
# RF-POWER-MONITOR-MIB::mainsPowerLastChange.0 = Timeticks: (0) 0:00:00.00
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

- Up to 16 variable bindings per request and at most 16 in a GetBulk response. A larger GetBulk reply would let a small request with a faked source address make the device send a much larger packet to a victim (reflection); a bulk walk simply takes more requests (at least 16 for the 241 objects of a walk from the root). A GetBulk request also does not make its source trusted for the rate limiter.
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
| IP address tables (rows that change at run time) | `lib/snmp_agent/snmp_mib` (dynamic columns), `src/net_stats.cpp` | `test/native/test_snmp_agent`, `test/hil/test_snmp.py`, `test/hil/test_ipv6.py` |
| IP, ICMP, TCP, UDP counters | lwIP `MIB2_STATS` (`include/lwipopts.h`), `src/net_stats.cpp` | `test/native/test_snmp_agent`, `test/hil/test_snmp.py` |
| CPU load (one-minute average per core) | `lib/cpu_load`, `src/host_stats.cpp` | `test/native/test_cpu_load`, `test/hil/test_host_resources.py` |
| HOST-RESOURCES-MIB, ENTITY-MIB values | `lib/snmp_agent/snmp_mib`, `src/host_stats.cpp` | `test/native/test_snmp_agent`, `test/hil/test_host_resources.py` |
| Traps on the device | `src/snmp_service.cpp` | `test/hil/test_snmp_traps.py` |
| Against real net-snmp, without hardware | `test/host/host_agent.cpp` | `scripts/host_agent_check.sh` |
| UDP port 161 on the device | `src/snmp_service.cpp` | `test/hil/test_snmp.py` |

To add an object: add a getter and an entry in `lib/snmp_agent/src/snmp_mib.cpp` (keeping the table sorted), extend `test_mib_contains_expected_objects` and the walk checks, add it to the MIB file and to this table.
