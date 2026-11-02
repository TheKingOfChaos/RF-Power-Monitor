# Standard MIBs: CPU, memory, location and the rest (design)

Date: 2026-11-02. Status: for approval.

## Why

In LibreNMS the device shows no location, no CPU load and no memory use (the user's words: "I can't see location and CPU and mem use"). The user also asked which standard SNMP objects the agent is missing, and wants all of them except linkUp/linkDown traps.

- **Location:** the agent already serves sysLocation and sysContact, but both are empty on the device, and only the serial menu can set them.
- **CPU and memory:** LibreNMS finds these on any device through the standard HOST-RESOURCES-MIB, which the agent does not serve.

Success: after a rediscovery in the local LibreNMS (`librenms/test`, http://localhost:8070), the device's pages show the location, a processor graph per core, a memory graph, the LittleFS storage, the IPv4 and IPv6 addresses on eth0, an Inventory entry and the SNMP/IP/UDP/ICMP statistics graphs. None of this should need special handling in our LibreNMS files beyond adding graphs to the overview.

## Goals

1. **Web page:** sysContact and sysLocation can be edited on the web page, in the form that already has the hostname (POST `/hostname`, or a renamed form). They use the same rules as menu option 4: 0-63 printable characters, empty allowed, `settingsTextIsValid`. Saved and active at once, like the hostname.
2. **SNMPv2-MIB (RFC 3418), complete:**
   - `sysORLastChange` (1.3.6.1.2.1.1.8.0, TimeTicks 0) and `sysORTable` (1.9.1.2-4.n), one row per MIB the agent implements: SNMPv2-MIB, IF-MIB, IP-MIB, UDP-MIB, TCP-MIB (if served, see 5), HOST-RESOURCES-MIB, ENTITY-MIB, LLDP-MIB, RF-POWER-MONITOR-MIB. Each row has the module-identity OID, a description and sysORUpTime 0.
   - The snmp group 1.3.6.1.2.1.11: every object RFC 1213 and RFC 3418 define there (snmpInPkts .1 through snmpProxyDrops .32, skipping the numbers that were never assigned, 7 and 23). Real values come from `SnmpStats` and `SnmpServiceStats`, extended where needed (e.g. snmpOutPkts, snmpInTotalReqVars, snmpOutTraps). snmpSilentDrops keeps its RFC 3418 meaning: requests dropped because not even an empty response fit (the agent's `SnmpOutcome::NoRoom`). Rate-limited drops have no standard counter and stay on the status screen only (my call). Counters for events that cannot happen (snmpInSetRequests can, but snmpInTraps, snmpInGetResponses, snmpOutGetRequests, ... are always 0) stay at 0. `snmpEnableAuthenTraps` = 1 (enabled, read-only: the device does send authenticationFailure).
   - `snmpSetSerialNo` (1.3.6.1.6.3.1.1.6.1.0, TestAndIncr): read-only, value 0. RFC 3418 requires it; the device accepts no Sets, so it never changes.
3. **HOST-RESOURCES-MIB (RFC 2790):**
   - hrSystem: `hrSystemUptime` (same as sysUpTime), `hrSystemNumUsers` 0, `hrSystemProcesses` 2 (the two core loops), `hrSystemMaxProcesses` 2. Not `hrSystemDate`: the device has no clock (no NTP).
   - `hrMemorySize` (KB of RAM).
   - `hrStorageTable`, two rows:
     - **RAM** (`hrStorageRam`): size = the RP2040's 256 KB main SRAM in 1-byte or 1 KB units (the size the build reports, 262144 bytes), used = size minus free heap. This counts static data, the stacks and the heap, so it matches what the build's "RAM: 45.4%" means plus what the heap uses while running.
     - **LittleFS** (the 0.25 MB settings partition): size and used from LittleFS (`FSInfo`). Type `hrStorageFlashMemory`, or `hrStorageFixedDisk` if LibreNMS's storage module skips flash memory (check against the local LibreNMS).
   - `hrDeviceTable` with two processor rows (`hrDeviceProcessor`, descr e.g. `RP2040 core 0 (network, SNMP)` / `RP2040 core 1 (mains input)`, status running), and `hrProcessorTable` for both (`hrProcessorFrwID` 0.0, `hrProcessorLoad`).
   - **hrProcessorLoad:** per RFC 2790, "the average, over the last minute, of the percentage of time that this processor was not idle", integer 0-100.
4. **ENTITY-MIB (RFC 6933 / 4133):** one `entPhysicalTable` row, the device itself (class chassis): descr, vendor type (our sysObjectID), contained-in 0, name, hardware revision `W5500-EVB-Pico-PoE + carrier rev 1.1` (a build constant, next to FIRMWARE_VERSION in `include/version.h`, to change with the board), firmware revision (FIRMWARE_VERSION), serial number (the RP2040's flash unique ID as hex), manufacturer `Kenneth Foli Jørgensen` (as in the carrier's KiCad title block; the user agreed), model name `RF-Power-Monitor`, isFRU false. The strings are UTF-8 (SnmpAdminString). Check that LibreNMS's generic discovery does not overwrite the hardware/version the `sysDescr_regex` sets today, or that what it shows instead is fine. Optionally a second row for the Ethernet port (class port, contained in 1) if that is what LibreNMS needs to link the port to the inventory; otherwise one row.
5. **IP-MIB (RFC 4293), UDP-MIB, ICMP and TCP statistics:**
   - `ipAddrTable` (IPv4, the one the LibreNMS IPv4 discovery reads): the device's IPv4 address, ifIndex 1, netmask, broadcast bit, reassembly size.
   - `ipAddressTable` for IPv4 and every IPv6 address the netif has (link-local, SLAAC, DHCPv6, static: at most `LWIP_IPV6_NUM_ADDRESSES` = 4), with ifIndex, type unicast, prefix (`ipAddressPrefix` RowPointer, plus `ipAddressPrefixTable` if LibreNMS needs it to read the prefix length), origin (manual / dhcp / linklayer / random as fits), status preferred, row status active, storage type volatile. Only addresses in a valid state appear.
   - The scalar statistics LibreNMS's netstats graphs read: IP (ipForwarding 2 = not forwarding, ipDefaultTTL, ipInReceives, ipInDelivers, ipOutRequests, ...), ICMP (icmpInMsgs, icmpOutMsgs, echo counts), UDP (udpInDatagrams, udpNoPorts, udpInErrors, udpOutDatagrams), TCP (tcpActiveOpens, tcpPassiveOpens, tcpCurrEstab, tcpInSegs, tcpOutSegs, ...). See "Counters we cannot measure" under Constraints.
6. **LibreNMS:** add the processor, memory and storage graphs to the device overview (`over:` in `librenms/os_detection/rfpowermonitor.yaml`), and fill the OS `serial` from ENTITY-MIB if the generic discovery does not. Rediscover the device in the local LibreNMS and check that every item under "Success" appears.

## Non-goals

- linkUp/linkDown traps (the user's decision: a linkDown trap cannot leave while the link is down).
- SNMP Set of any object, including sysContact/sysLocation over SNMP and snmpSetSerialNo. Everything stays read-only.
- SNMPv3 and its MIBs (SNMP-FRAMEWORK-MIB snmpEngineID etc.).
- hrSystemDate, hrSWRun/hrSWInstalled, hrFSTable, hrPartition: no clock, no processes or filesystems worth listing.
- ENTITY-SENSOR-MIB: the temperatures stay in our own MIB, where LibreNMS already reads them.
- Route or ARP tables (ipCidrRouteTable, ipNetToPhysicalTable).

## Anti-goals (would be a failure even if it works)

- v1 behaviour changes for existing objects, or an existing object changes value or OID.
- SNMP requests get slower than 100 ms or measurably slower than today (~4 ms median), or a walk no longer finishes because the MIB lookup got expensive.
- The CPU load measurement itself loads the CPU or delays mains detection (core 1 must still detect within 10 ms of GPIO27 changing).
- Numbers that look real but are not: a counter we cannot measure is 0 and documented as such, never estimated.
- RAM over 50%. The user allowed up to 60% if needed. Use that only if the design cannot fit under 50%, say so, and update `.clinerules` (Resource Usage) in the same commit.

## Constraints

- All `.clinerules` rules apply: pure logic in `lib/` with native tests; tests first; truncated/corrupted input tests for anything parsed; sanitizers; net-snmp captured vectors where possible; HIL tests for what is visible over the network; docs (docs/MIB.md, MIB comments in `snmp_mib.h`, README/USER_MANUAL/SETUP_MANUAL for the web fields), TODO.md and CHANGELOG.md updated; version bump (new features: 3.1.0).
- No buffer or struct over ~256 bytes on the stack. Core 0 stack headroom must stay as the HIL stack test expects.
- **Dynamic rows:** the MIB today is one sorted static table of fixed OIDs (`lib/snmp_agent/src/snmp_mib.cpp`). The IPv4 address, the IPv6 addresses and the prefixes are part of row OIDs and change at run time (DHCP, SLAAC, DHCPv6). The agent needs a way to serve such rows with correct Get/GetNext/GetBulk ordering, in the pure library and host tested, including empty tables and address changes between two GetNext calls. The data comes in through `SnmpDeviceState`, as for LLDP. IPv6 address rows are 28 arcs long (fits `OID_MAX_ARCS` = 32).
- **CPU load:** neither loop idles in a way that can simply be timed. Core 1 sleeps in `delay(1)` each pass, so its busy time is the time outside that delay. Core 0 busy-polls without sleeping, so "not idle" needs a definition: e.g. time beyond the cost of a pass in which nothing had work, or loop passes per second compared with a calibrated idle rate. The builder picks the method. Requirements:
  - 0-5% on an idle device.
  - Clearly higher during an SNMP bulk walk or a web page load burst.
  - Never above 100.
  - One-minute averaging in a pure, host-tested library.
  - Sampling cost small enough not to change the HIL timing tests.
- **Protocol statistics (IP/ICMP/UDP/TCP):** these come from lwIP's exact MIB-II counters (`MIB2_STATS`), turned back on at the user's request (2026-11-02). The core's `lwipopts.h` sets `LWIP_STATS 0` unconditionally, so our `include/lwipopts.h` loads it with `#include_next` and overrides it. A spike built, and every lwIP module counted.
- **Counters we cannot measure** (my call, one rule for every group):
  - A counter for an event that cannot happen on this device (snmpInTraps, ipForwDatagrams, ...) is served as 0. That is its true value.
  - A counter for an event that does happen but the firmware cannot see is also served as 0, and docs/MIB.md says it is not measured, like ifOutDiscards today. This keeps groups complete so LibreNMS's graphs find every object they expect.
  - A whole group is left out (objects, sysORTable row and docs) only when its main counters cannot be measured. For TCP, that means segments in and out and the connection counts.
- Network-visible changes need HIL tests: walks with the new objects in v1 and v2c (update the expected object counts in docs/MIB.md and the tests), CPU load rising under load, RAM/LittleFS values plausible, IPv6 addresses matching the status screen, web form round trip for contact/location.
- `scripts/host_agent_check.sh` (net-snmp against the host build) covers the new objects. The standard MIBs resolve names via snmp-mibs-downloader.

## Decided

- All of the above except linkUp/linkDown traps (user).
- LittleFS appears as a storage disk (user).
- CPU load per core, two processors (my guess, confirmed by the user's "yes").
- RAM limit: stay under 50%; 60% is allowed only if needed (user).
- ENTITY-MIB manufacturer is the user's name (user).
- My calls (from the review): unmeasurable counters are 0 and documented, and a group is left out only when its main counters cannot be measured; snmpSilentDrops keeps its RFC meaning; the hardware revision is a build constant.

## Left to the builder

- Exact text of descriptions (hrDeviceDescr, sysORDescr, entPhysicalDescr).
- The CPU load method within the constraints above.
- Whether the dynamic-row support is a second table kind in `snmp_mib.cpp` or a separate module.
- Storage units (hrStorageAllocationUnits).
- The ENTITY-MIB port row, IP-MIB prefix table, and which protocol counters are served, decided by checking what the local LibreNMS (26.10.2) actually reads.
- Splitting the work into commits. Suggested order: web fields → SNMPv2-MIB → HOST-RESOURCES (RAM, LittleFS, CPU) → ENTITY-MIB → dynamic rows + IP-MIB → protocol stats → LibreNMS.
