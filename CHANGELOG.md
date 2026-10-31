# Changelog

All notable changes to the RF-UPS-Monitor firmware are recorded here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses [Semantic Versioning](https://semver.org/): the version is `FIRMWARE_VERSION` in [include/version.h](include/version.h), shown in the boot banner and in sysDescr, and each release has a git tag `vX.Y.Z`.

## [Unreleased]

### Added
- **Ambient temperature** from the TMP1075 on the carrier board (I2C0, GP4/GP5, address 0x48), read every 2 s: SNMP `ambientTemperature` (`1.3.6.1.4.1.63050.5.0`, 0.1 °C), the status screen, the web page and a LibreNMS sensor "Ambient (TMP1075)" (warning 40 °C, critical 50 °C). Without the sensor (module without the carrier board, or three failed reads in a row) the object has no instance: v2c Get answers noSuchInstance, v1 Get noSuchName, and walks skip it; the status screen and the web page say "no sensor".
- Web page: with DHCP the Network card's boxes show the IP address, subnet mask and gateway in use (from the DHCP server), with the DNS server below them; choosing Static and saving keeps that address. Saving with DHCP still keeps the stored static settings. Before, the boxes showed the stored static settings, which are not in use with DHCP.
- Hardware: 3D-printable case for the module on the carrier: Fusion 360 design `hardware/Case/Power Case.f3z`, STEP files for the case and the lid, and the print project `hardware/pico-poe-mains-carrier/3d/Power Case.3mf`.

### Fixed
- Factory reset button: "released after ... ms, no reset" always said 0 ms; it now gives how long the button was held.
- HIL factory reset test: during the 10 s hold it polled SNMP with "public" after changing the community, so it never got a reply; it uses the community it set, and it reconnects before restoring the settings when the device rebooted mid-test.

### Changed
- The RP2040 die temperature is called the **CPU temperature**: status screen, web page, LibreNMS sensor "CPU (RP2040 die)", and the MIB object `boardTemperature` is renamed `cpuTemperature`. The OID (`63050.4.0`) and the value are unchanged, so numeric polling keeps working; scripts that use the name `RF-UPS-MONITOR-MIB::boardTemperature` need the new name. The LibreNMS sensor keeps its index, so its graph history stays.
- Factory reset button tested on the carrier board (SW1 on GP22): a short press is ignored, a 10 s hold resets after 10000-10100 ms and reboots within 2 s.
- The status screen prints temperatures between 0 and -1 °C with their minus sign (it showed -0.5 °C as 0.5 °C).
- Web page buffers (`WEB_BODY_MAX`) 8192 -> 8704 bytes for the temperature rows; the largest status page is 8354 bytes. RAM use 42.2 %.
- Hardware: F1 is a SETfuse SCT1032 (T 1 A, 250 VAC, 100 A breaking capacity, LCSC C19712559) on the same Schurter UMT 250 footprint, so JLCPCB can place it; it was a Schurter T 100 mA without an LCSC part. Fab files, renders and 3D models regenerated.

## [2.4.2] - 2026-10-19

The firmware is unchanged apart from its version number.

### Changed
- Project name is RF-UPS-Monitor (without "v3", which looked like a version number). The firmware already reported this name in sysDescr; the README, CLAUDE.md and the test fixtures follow.
- Hardware: carrier board v1.1. F1 (T 100 mA, 250 VAC, Schurter UMT 250) after the L terminal; the divider is now R1 in L and R2-R4 in N, one series loop through F1. SW1 factory reset button from GP22 to GND, so the firmware's 10 s factory reset works on the carrier. The I2C pull-ups are renumbered R8/R9 (no gap after R7). The silkscreen carries the RF logo (from `logo/`, flattened to a filled polygon by `tools/generate.py`), "RF-UPS-Monitor v1.1" and "Made by Kenneth Foli Jørgensen 2026"; the schematic and board title blocks name RF-UPS-Monitor, the maker and the year. Two more M3 holes, H4 and H5, under the module between the header rows, for mounting the carrier when the module is on headers (a project footprint without courtyard, so the courtyard check stays on for the parts). ERC and DRC clean; fab files, renders and 3D models regenerated.

## [2.4.1] - 2026-10-18

The firmware is unchanged apart from its version number.

### Added
- Hardware: carrier PCB for the W5500-EVB-Pico-PoE in `hardware/pico-poe-mains-carrier/` (KiCad 10 project, fab files for JLCPCB, 3D models, renders), documented in the README with how it maps to the firmware. Open points are in TODO.md: the TMP1075 is not read yet, nothing is on GP22 for the factory reset (the module's buttons are BOOTSEL and RUN), and the detector reports a mains loss about 30-50 ms late.
- Diagrams: web page request flow and where the IPv4/IPv6 addresses come from.
- LibreNMS over IPv6 checked (transport `udp6`, LibreNMS 26.9.1); the Docker test network has IPv6.

### Changed
- Docs: web page section with every feature, the URLs for scripts and troubleshooting; static IPv6 in the settings file and factory reset; project structure.

## [2.4.0] - 2026-10-17

### Added
- **Static IPv6 address and gateway:** serial menu option 9 and an IPv6 card on the web page. One address (/64), added next to the link-local and SLAAC addresses, with duplicate address detection; an optional gateway that becomes the next hop for all off-link IPv6 traffic (lwIP hook `LWIP_HOOK_ND6_GET_GW`, `include/lwip_hooks.h`), otherwise the advertised routers are used. Addresses are checked (no link-local, multicast, ::/8 or zero interface id; gateway link-local or in the /64) and stored in RFC 5952 form. The status screen marks the address `(static)` or `(DUPLICATE ...)` and shows the IPv6 gateway.
- Settings: optional `ipv6Address` and `ipv6Gateway` fields; files from older firmware load unchanged (empty = none), a factory reset clears them.

### Changed
- lwIP keeps 4 IPv6 addresses (`LWIP_IPV6_NUM_ADDRESSES=4`, was 3): link-local, static and two advertised prefixes.
- `SETTINGS_JSON_MAX` 896 -> 1024 bytes for the two new fields.
- The web page's status data (~340 bytes) is a member of the page object instead of a local variable, keeping it off core 0's stack.
- IPv6 text parsing and RFC 5952 formatting live in `lib/netconfig` (`parseIPv6`, `formatIPv6`); the access log uses the same formatter.

## [2.3.0] - 2026-10-17

### Added
- **IPv6 (dual stack):** link-local and SLAAC addresses; SNMP, the web page and mDNS work over IPv6. The status screen and the web page show the IPv6 addresses.
- Rate limiting, trust and the access log work per full source address for IPv6 as for IPv4 (the log shows `fd16:d833:4f0e:3ece:9000:9aaa:490a:3189`); IPv4-mapped addresses count as IPv4, sources in ::/32 are dropped. Not per /64: every host on a LAN shares one /64, so a manager's trust would have passed to all its neighbours (found in review before release).
- Diagnosis build `-DRFUPS_DIAG -Wl,--wrap=panic`: hardware watchdog, loop/hook breadcrumbs, hard-fault PC and panic message reported after the restart (docs/TESTING.md).

### Fixed
- With IPv6, lwIP's timer pool (`MEMP_NUM_SYS_TIMEOUT`) ran empty a few seconds after boot because mDNS keeps timers for both IP versions and the core sizes the pool without mDNS; lwIP then panicked. The pool has 12 more slots (~200 bytes).
- With IPv6, two SNMP requests arriving back to back crashed the device. The core's `UdpContext` marks a helper buffer between queued UDP packets with flags `0xFF`, which includes lwIP's `PBUF_FLAG_IS_CUSTOM`; IPv6 fragmentation turns custom buffers on, and freeing the helper then jumped to garbage. IPv6 fragmentation is off (`LWIP_IPV6_FRAG=0`), so custom buffers are off as in the IPv4 build; IPv6 SNMP replies are limited to 1452 bytes (one frame), larger GetBulk replies are shortened. Found with the diagnosis build (hard fault in `pbuf_free`).
- With IPv6, a burst of web page loads left only ~660 bytes of core 0's 4 KB stack unused; an overflow would silently overwrite core 1's stack. Each core now has its own 8 KB stack (`core1_separate_stack`, 8 KB of RAM); ~5.3 KB stay unused after the same burst.

### Notes
- Earlier docs said lwIP is precompiled. It is compiled from source with the core's `lwipopts.h` and our build flags; the DHCP option 224 and port 80 workarounds still work, but could be replaced by lwIP options (TODO).

## [2.2.0] - 2026-10-17

### Added
- **Trap destination from DHCP:** enter `dhcp` as the trap destination (menu option 4). The device asks for DHCP option 224 in its DHCP messages and sends traps to the address in the ACK for its current lease. Works with any DHCP server that has option 224 configured; tested with Kea 2.2.0 (configuration examples for Kea, ISC dhcpd and dnsmasq in docs/MIB.md). New pure library `lib/dhcp_option`, tested with frames captured from the device and from Kea. Option 224 is only taken from an ACK lwIP accepted (same transaction id and server, lwIP bound by that frame), so a forged ACK from another LAN host cannot redirect traps (found by an automated security review; HIL test sends one).
- **Web interface on/off:** serial menu option 8 and a "Turn off…" button on the page (with confirmation). Off closes TCP port 80, logs every browser out and stops the mDNS announcement; kept across restarts, turned on again by option 8 or a factory reset. Turning it on works at once, also right after the page turned itself off: the core's lwIP is built without SO_REUSE, so a just-closed web connection in TIME-WAIT would block port 80 for 2 minutes; the firmware drops those entries for port 80 before reopening it, and retries every second if opening still fails (found by the HIL suite).
- **Logo** (`logo/RF-canopy-RGB-Orange.svg`) in the top bar, on the login card and as the browser tab icon (`/favicon.svg`).
- Settings fields `trapFromDhcp` and `webEnabled` (optional; older files load unchanged, older firmware ignores them).
- Status screen: `Trap destination source` and `Web interface` lines; the trap destination shown is the one in use.

## [2.1.1] - 2026-10-17

### Changed
- Web page restyled with Bootstrap 5.3.3: dark navigation bar, status card with green/red badges for mains power and the Ethernet link, settings in cards, Bootstrap forms and alerts, two columns on wide screens and one on phones. Bootstrap's CSS is built into the firmware (31 KB gzip in flash, served at `/static/bootstrap-5.3.3.min.css`, cached for a year, no login needed), so no internet access is needed. `scripts/embed_bootstrap.py` regenerates it from a pinned, integrity-checked download.
- Content-Security-Policy now allows the device's own stylesheet and Bootstrap's `data:` images; still no scripts.

## [2.1.0] - 2026-10-17

### Added
- **Hostname:** one name for SNMP sysName, the DHCP hostname (option 12) and mDNS: the device answers as `<hostname>.local` and announces the web page as `_http._tcp`. Set in serial menu option 4 ("Hostname (sysName)") or on the web page ("Device name"); changes apply immediately (mDNS rename, DHCP lease renewal), no restart.
- Status screen and web page show the hostname and the `.local` name.

### Changed
- New sysName values must be valid hostnames (1-32 letters, digits and `-`). A free-text sysName from older firmware still loads; DHCP and mDNS then use a cleaned-up version of it.
- A login with an own web password hashes 20 rounds per loop pass instead of 40 (about 5 ms), for more margin to the 100 ms SNMP limit.

## [2.0.1] - 2026-10-17

### Security
- Serial menu option 7 replaces the device default web password with a new random one, for when the default password became known (the README of 2.0.0 showed the development device's real default password as an example). The old password stops working at once and browsers logged in with it are logged out.

### Fixed
- README: the example default password is made up.

## [2.0.0] - 2026-10-17

A new major version for the second management interface: the configuration web page with login. SNMP, the MIB, the serial menu and the settings files stay compatible: files from 1.x load unchanged, and older firmware ignores the new `webPassword` field (a downgrade only loses the own web password).

### Added
- **Web page** on `http://<device-ip>/` with login (user `admin`): status, DHCP/static IP (saves and restarts), SNMP community (active immediately), restart button, log out. Pure library `lib/web_ui` (HTTP parser, sessions, pages, SHA-256) with host tests; `src/web_service.cpp` serves it without ever blocking SNMP.
- **Web password:** a random 10-character default password per device, generated on first boot, stored in `data/web_default.txt` and shown only on the serial status screen; serial menu option 6 sets an own password (stored as a salted, iterated SHA-256 hash in the new optional settings field `webPassword`) or returns to the default.
- Web security: 30 s lockout after 5 failed logins, HttpOnly SameSite=Strict session cookie, CSRF token in every form, idle sessions end after 15 min, strict Content-Security-Policy, no framing; failed logins and lockouts in the access log.
- The own-password check (2000 SHA-256 rounds, ~0.45 s on the RP2040) runs 40 rounds per loop pass, so a login never delays SNMP (measured: worst 52 ms during a login, before the change 467 ms); found by a code review before release.
- Status screen: web login line and the core 0 stack high-water mark (`Core 0 stack: N bytes never used`).
- Excalidraw diagrams in `docs/diagrams/` (system overview, firmware architecture, SNMP request flow, mains flow, settings storage, test layers), editable `.excalidraw` files with SVG exports shown in the README and docs/TESTING.md.

### Changed
- `Settings` grew by the password hash, so every temporary copy of it on core 0's 4 KB stack became static (serial menu, web page, settings parser). Settings save/load measures 1120 bytes of stack on the device (budget 1536).

## [1.0.2] - 2026-10-17

LibreNMS support tested in a real LibreNMS, and the mains tests automated. The firmware itself is unchanged apart from the version number.

### Fixed
- LibreNMS temperature sensor: low limit set to -20 C. Without it LibreNMS put the low limit 10 C below the first reading, which alarmed when the room cooled down.

### Added
- Documentation overview in the README, a real status screen example, and the trap settings in the settings file description.
- `librenms/test/compose.yml`: a throw-away LibreNMS in Docker for checking the definitions. Tested with LibreNMS 26.9.1 against the device (see docs/LIBRENMS.md), including a real mains cut raising and clearing the "State Sensor Critical" alert.
- Automated mains HIL tests: with `RFUPS_MAINS_SWITCH=dirigera` the tests switch an IKEA Dirigera smart plug feeding the mains detector (`test/hil/mains_switch.py`, `dirigera_outlet.py`). Mains loss/restore over serial and SNMP and the mains traps in v1 and v2c now pass with real mains, plus a new test that a ~1 s outage shows in mainsPowerChanges.

## [1.0.1] - 2026-10-17

### Added
- IF-MIB `ifInDiscards`: received frames the driver dropped because lwIP had no free buffer. Counted by a small subclass of the core's W5500 driver (`CountingW5500` in `src/net_stats.h`).
- IF-MIB `ifInErrors`: received frames shorter than the Ethernet header or longer than 1514 bytes.
- IF-MIB `ifInUnknownProtos`: received frames with an EtherType lwIP does not handle (IPv6 while IPv6 is off, LLDP, spanning tree, VLAN-tagged).
- Status screen line `Ethernet drops` with these counters and the send errors.

### Changed
- Received frames that lwIP drops no longer count as unicast/multicast/broadcast packets (IF-MIB counts only delivered packets there); their octets still count in ifInOctets.

### Known limits
- The W5500 has no counters for CRC errors or frames lost when its own receive buffer is full, so those are not visible. ifOutDiscards stays 0.

## [1.0.0] - 2026-10-17

First release.

### Added
- **Ethernet** with the W5500 on the Arduino-Pico core's lwIP driver: DHCP (default) or static IP, lease renewal, boot without waiting for the lease, and when no DHCP server answers the device keeps retrying instead of falling back to another address. MAC address derived from the chip ID.
- **Serial menu** on USB (115200 8N1): status, DHCP, static IP with validation, SNMP settings, access log. Echo and backspace for terminals without local echo.
- **Settings** as JSON on LittleFS in two copies (A/B) with a sequence number and read-back check, so a power cut during a save never loses them. Factory defaults when no copy is valid; files from older firmware still load.
- **Mains power monitoring** on GPIO27: sampled at 1 kHz on core 1, debounced 5 ms, change detected within 10 ms, handed to core 0 through a sequence lock; logged on the serial port and shown on the status screen.
- **Factory reset** with the GPIO22 button: 10 s hold, non-blocking (sampled on core 1), LED feedback, ignored when held at boot.
- **SNMP agent** (own code) on UDP 161, read-only, SNMPv1 and SNMPv2c; SNMPv3 is dropped and counted.
  - Get, GetNext and in v2c GetBulk; v2c exceptions (noSuchObject, noSuchInstance, endOfMibView) and errors (notWritable, noCreation, tooBig with an empty list).
  - System group with sysName, sysContact and sysLocation set from the menu, active without a reboot.
  - RF-UPS-MONITOR-MIB (enterprise 63050): mainsPowerStatus, mainsPowerChanges, mainsPowerLastChange, boardTemperature; MIB file in `mibs/`, validated with net-snmp.
  - IF-MIB ifTable and ifXTable for the Ethernet port, with 64-bit counters (Counter64, v2c only) counted by the firmware for every frame, and link state and speed from the W5500 PHY.
  - Board temperature from the RP2040's internal sensor, in 0.1 °C.
- **Traps** in SNMPv1 or SNMPv2c (setting, default v2c): coldStart, authenticationFailure (at most one per 10 s), mainsPowerLost and mainsPowerRestored with the mains objects.
- **Security**: per-source rate limit (50/s, burst 100), a global limit for unauthenticated sources (100/s) so faked source addresses cannot bypass it, a bounded separate limit for sources that know the community (200/s, at most 4, expires after 10 minutes idle, revoked when the community changes), GetBulk replies capped at 16 objects against reflection, and an access log of rejected requests (menu option 5 and serial log).
- **LibreNMS** OS detection and discovery definitions in `librenms/` (mains state sensor, temperature sensor, change counter), see [docs/LIBRENMS.md](docs/LIBRENMS.md).
- **Tests**: native unit tests for every pure library, on-device tests, hardware-in-the-loop tests over the serial menu and net-snmp, AddressSanitizer/UBSan runs, the agent checked against the real net-snmp tools without hardware, and GitHub Actions CI. Test rules in `.clinerules`, guide in [docs/TESTING.md](docs/TESTING.md).

### Known limits
- The network stack queues 5 UDP packets; larger bursts are dropped (SNMP managers retry).
- Not yet tested with the real hardware: mains traps with real mains, the factory reset button, the "no link" and "no DHCP server" states. The LibreNMS definitions are validated against the schema but not yet tried in a running LibreNMS (done after 1.0.1).

[Unreleased]: https://github.com/TheKingOfChaos/RF-UPS-Monitor-v3/compare/v2.4.2...HEAD
[2.4.2]: https://github.com/TheKingOfChaos/RF-UPS-Monitor-v3/compare/v2.4.1...v2.4.2
[2.4.1]: https://github.com/TheKingOfChaos/RF-UPS-Monitor-v3/compare/v2.4.0...v2.4.1
[2.4.0]: https://github.com/TheKingOfChaos/RF-UPS-Monitor-v3/compare/v2.3.0...v2.4.0
[2.3.0]: https://github.com/TheKingOfChaos/RF-UPS-Monitor-v3/compare/v2.2.0...v2.3.0
[2.2.0]: https://github.com/TheKingOfChaos/RF-UPS-Monitor-v3/compare/v2.1.1...v2.2.0
[2.1.1]: https://github.com/TheKingOfChaos/RF-UPS-Monitor-v3/compare/v2.1.0...v2.1.1
[2.1.0]: https://github.com/TheKingOfChaos/RF-UPS-Monitor-v3/compare/v2.0.1...v2.1.0
[2.0.1]: https://github.com/TheKingOfChaos/RF-UPS-Monitor-v3/compare/v2.0.0...v2.0.1
[2.0.0]: https://github.com/TheKingOfChaos/RF-UPS-Monitor-v3/compare/v1.0.2...v2.0.0
[1.0.2]: https://github.com/TheKingOfChaos/RF-UPS-Monitor-v3/compare/v1.0.1...v1.0.2
[1.0.1]: https://github.com/TheKingOfChaos/RF-UPS-Monitor-v3/compare/v1.0.0...v1.0.1
[1.0.0]: https://github.com/TheKingOfChaos/RF-UPS-Monitor-v3/releases/tag/v1.0.0
