# RF-UPS-Monitor TODO

An item is only ticked when it is implemented **and** its tests exist and pass (see `.clinerules`, Test Rules).

## 1. Network
- [x] W5500 bring-up via the core's lwIP driver (`Wiznet5500lwIP`), hardware reset on GPIO20
- [x] DHCP client (lwIP), lease renewal — `test/embedded/test_ethernet`, HIL
- [x] Static IP from settings — HIL `test_static_ip_applies_after_reboot` (passed with 10.10.6.249, 2026-10-16)
- [x] Boot without a serial terminal attached (3 s wait max)
- [x] No lease: keep retrying DHCP, no fallback address (decided 2026-10-16, avoids IP conflicts)
- [x] Boot does not wait for DHCP, menu available immediately — HIL `test_menu_is_available_before_dhcp_lease`
- [x] Status shows connection state (connected / waiting for lease / no link / no W5500) — HIL
- [ ] Test the "no link" and "no DHCP server" states (needs a switchable port or isolated network)
- [x] Delete the obsolete custom stack (`lib/w5500`, `lib/network`) and its tests

## 2. SNMP v1/v2c agent
### 2.1 BER codec (pure library, native tests) — `lib/snmp_ber`, `test/native/test_snmp_ber`
- [x] Length encoding/decoding (short and long form, minimal on encode)
- [x] INTEGER, OCTET STRING, NULL, OBJECT IDENTIFIER, SEQUENCE, IpAddress
- [x] TimeTicks, Counter32, Gauge32
- [x] OID text parsing/formatting, lexicographic compare for GetNext
- [x] Reject malformed and truncated input, bounds checks on every read
- [x] Byte-exact against requests captured from net-snmp
- [x] Clean under AddressSanitizer/UBSan incl. every single-byte corruption — `scripts/sanitize.sh`
### 2.2 Message handling — `lib/snmp_agent/snmp_message`, `test/native/test_snmp_message`
- [x] Parse Message (version 0, community, PDU), max 16 varbinds, trailing bytes rejected
- [x] v2c/v3, response/trap PDUs and malformed packets reported separately
- [x] Build GetResponse; error responses echo the request varbinds (RFC 1157)
- [x] Byte-exact against net-snmp requests, every truncation and single-byte corruption
### 2.3 Operations — `lib/snmp_agent/snmp_agent`, `test/native/test_snmp_agent`
- [x] Community check (constant time), wrong community silently dropped and counted
- [x] GetRequest, noSuchName with error-index for unknown or non-leaf OIDs
- [x] GetNextRequest, lexicographic order, noSuchName past the end (net-snmp: "End of MIB")
- [x] SetRequest answered with noSuchName (everything read-only)
- [x] tooBig when the response does not fit; statistics counters
### 2.4 MIB — `lib/snmp_agent/snmp_mib`, `mibs/RF-UPS-MONITOR-MIB.txt`, `docs/MIB.md`
- [x] system group: sysDescr, sysObjectID, sysUpTime, sysContact, sysName, sysLocation, sysServices
- [x] 63050.1 mainsPowerStatus, .2 mainsPowerChanges, .3 mainsPowerLastChange
- [x] OID table sorted (tested), full walk test
- [x] Host build checked against real snmpget/snmpwalk/snmpset — `scripts/host_agent_check.sh`
- [x] MIB file validated with snmptranslate `-PW` (no warnings), names resolve in snmpget — `host_agent_check.sh`
- [x] sysContact / sysLocation / sysName configurable (settings + menu option 4), active without reboot — native `test_settings_codec`, HIL `test_snmp_settings_*`
### 2.5 Integration — `src/snmp_service.cpp`, `test/hil/test_snmp.py`
- [x] UDP port 161 on lwIP, max 4 packets per loop, oversized packets dropped
- [x] HIL tests with net-snmp from WSL: get, getnext, walk, set, wrong community, v2c
- [x] Response time < 100 ms (measured median ~6 ms, max ~20 ms over 30 requests)
- [x] Survives malformed packet floods; 5 simultaneous requests all answered
- [x] SNMP counters on the status screen
- [x] Mains loss/restore over SNMP with real mains — HIL `test_mains_loss_and_restore_visible_over_snmp`, `test_short_outage_between_polls_is_seen_in_the_change_counter` (Dirigera outlet, passed 2026-10-17)
- [x] Known limit, accepted and documented in docs/MIB.md: the core queues 5 UDP packets (`UdpContext::rxBufMaxDepth`, hard-coded), larger bursts are dropped; one reply is held during an ARP lookup. Managers poll one request at a time and retry
### 2.7 SNMPv2c (decided 2026-10-17: v1 and v2c, no v3; traps in v1 or v2c)
- [x] Parse and answer v2c (version 1), each request in its own version; v3 dropped and counted — native + HIL
- [x] v2c exceptions: noSuchObject / noSuchInstance (Get), endOfMibView (GetNext) — native, net-snmp, HIL
- [x] GetBulkRequest: non-repeaters, max-repetitions, early stop at endOfMibView, truncation to fit (max 16 varbinds, see 8), tooBig only if nothing fits — native, net-snmp `snmpbulkget`/`snmpbulkwalk`, HIL
- [x] v2c Set: notWritable / noCreation; v2c tooBig with empty varbind list — native, net-snmp
- [x] v1 behaviour unchanged — native `test_v1_behaviour_is_unchanged_for_the_same_requests`, all v1 tests still pass
- [x] SNMPv2-Trap (sysUpTime.0, snmpTrapOID.0, objects), byte-exact against `snmptrap -v2c` — native
- [x] Trap version setting (default v2c), menu option 4, status screen — native + HIL; trap tests run in both versions
- [x] v2c and GetBulk counters on the status screen
### 2.8 IF-MIB and board temperature (for LibreNMS)
- [x] Counter64 in the BER codec — native
- [x] Frame counting (octets, unicast, multicast, broadcast, 64-bit) and W5500 PHYCFGR decoding, `lib/if_stats` — native `test_if_stats`
- [x] Counting hooked into the lwIP netif input/linkoutput, PHY read every 500 ms, link changes logged, `src/net_stats.cpp` — HIL `test_traffic_counters_count_real_packets`, `test_if_speed_and_mac_match_the_device`
- [x] ifNumber, ifTable (22 columns), ifXTable (19 columns) for interface 1 — native, net-snmp `host_agent_check.sh`, HIL
- [x] Counter64 hidden from v1 requests (RFC 3584) — native, net-snmp, HIL
- [x] boardTemperature (63050.4, 0.1 °C, RP2040 sensor averaged) in the agent and the MIB file — native, net-snmp, HIL
- [x] Status screen: board temperature, Ethernet link and traffic — HIL
- [x] LibreNMS OS detection/discovery definitions (`librenms/`), validated against the LibreNMS schemas — `docs/LIBRENMS.md`
- [x] Tried in LibreNMS 26.9.1 (Docker, `librenms/test/compose.yml`) against the device: OS/version/hardware, three sensors, port and rates, unhandled trap in the event log
- [x] Real mains cut (Dirigera outlet) raises LibreNMS's "State Sensor Critical" alert on the next poll, recovers after mains returns
- [x] Temperature low limit set to -20 C (LibreNMS's automatic one was 10 C below the first reading)
- [x] RF logo as LibreNMS device icon and device page logo (`librenms/images/`) — LibreNMS in Docker, after a poll
- [x] v2c traps carry snmpTrapAddress.0 (RFC 3584) — native byte-exact vs net-snmp, HIL decoder, LibreNMS in Docker
- [x] Traps into the Docker LibreNMS (snmptrapd container, the trap's own address for Docker's NAT) — real mains cut, mainsPowerLost/Restored in the event log; a mains trap polls the sensors, so the alert opens/recovers in ~3 s
- [x] LibreNMS alerts only on outages over 60 s: `mainsPowerAlarm` (63050.6.0) raised after 60 s of lost mains, one `mainsPowerLostTooLong` trap (63050.0.3, v1 specific 3) per outage, held until a trap can leave; "Mains lost over 60 s" state sensor (critical), "Mains power" lost is a warning; no PHP handler upstream needed (a rule delay only holds back notifications) — native `test_mains_alarm`, `test_snmp_trap` byte-exact vs net-snmp, `test_snmp_agent`, `test_web_ui`; host_agent_check; HIL `test_short_mains_loss_does_not_raise_alarm`, `test_mains_loss_over_60_s_raises_alarm_until_restore`, `test_mains_alarm_trap_after_60_s`; LibreNMS with real mains cuts (33 s: no alert; 76/95 s: alert 2 s after the trap)
- [x] ifInDiscards (driver drops for lack of a buffer, via `CountingW5500`), ifInErrors (impossible lengths), ifInUnknownProtos (EtherType) — native `test_if_stats`, `test_if_receive_discards_errors_and_unknown_protocols`, net-snmp, HIL `test_receive_drop_counters_match_the_status_screen`
- [ ] Not possible with the W5500: CRC errors and W5500 receive-buffer overflows (no counters in the chip); ifOutDiscards (lwIP drops before the driver)
### 2.6 Traps — `lib/snmp_agent/snmp_trap`, `test/native/test_snmp_trap`, `test/hil/test_snmp_traps.py`
- [x] SNMPv1 Trap-PDU encoding, byte-exact against net-snmp `snmptrap`
- [x] coldStart once per boot when the network comes up — HIL (after reboot, empty ARP table)
- [x] authenticationFailure on wrong community, throttled to one per 10 s — native + HIL
- [x] Trap destination in settings and menu option 4, `-` turns traps off — native + HIL
- [x] Notifications in the MIB file (63050.0.1 / .0.2), validated with snmptranslate
- [x] Trap counters on the status screen

## 3. Power monitoring
- [x] Debouncer (5 ms), detection within 10 ms — `test/native/test_power_debouncer`
- [x] GPIO27 sampled at 1 kHz on core 1, seqlock hand-over to core 0 — `test/embedded/test_mains_input`
- [x] Status screen line and serial log on every change — HIL `test_status_reports_mains_power`
- [x] Verify loss/restore with real mains — HIL `test_mains_loss_and_restore_are_reported` (manual, `RFPOWER_MANUAL_MAINS=1`, passed 2026-10-16)
- [x] Expose in MIB: mainsPowerStatus, mainsPowerChanges, mainsPowerLastChange
- [x] SNMP trap on change: mainsPowerLost / mainsPowerRestored with the three mains objects — native byte-exact test
- [x] Mains traps with real mains, v1 and v2c — HIL `test_mains_traps_with_real_mains` (Dirigera outlet, passed 2026-10-17)
- [x] Mains tests automated with an IKEA Dirigera smart plug (`RFPOWER_MAINS_SWITCH=dirigera`, `test/hil/mains_switch.py`); manual mode kept

## 4. Configuration storage
- [x] LittleFS mount
- [x] JSON settings with validation (`lib/settings_codec`) — `test/native/test_settings_codec`
- [x] A/B copies with sequence number, power-cut safe — `test/embedded/test_settings_store`
- [x] Read-back verification on save
- [x] Load v1 files without `sequence`, remove old `active_file`
- [x] Factory defaults when no copy is valid
- [x] Store sysName / sysLocation / sysContact (optional fields, older files load with defaults)
- [x] Save/load stack use measured on the device (≤ 1.5 KB; core 0 has 8 KB since 2.3.0) — `test_save_and_load_use_little_stack`
- [x] Remove leftover `config.bak` / `config.schema.json` from older firmware at boot — `test_legacy_files_are_removed_and_settings_kept`
- [x] Trap destination setting (optional field, IPv4 or empty) — native

## 5. Serial menu
- [x] Show status (mode, stored static config, live interface)
- [x] Switch to DHCP
- [x] Set static IP with validation (`lib/netconfig`) — `test/native/test_netconfig`, HIL
- [x] Confirm, save, reboot; cancel leaves settings unchanged — HIL
- [x] Echo and backspace for terminals without local echo — HIL
- [x] Set SNMP community string (menu option 4, old community refused immediately) — HIL
- [x] Set sysName / sysLocation / sysContact (Enter keeps, `-` clears, length and character checks) — HIL
- [x] Over-long input lines rejected instead of silently truncated — HIL
- [x] Show SNMP statistics (on the status screen)
- [x] Trap destination in option 4 — HIL
- [x] Access log (option 5) — HIL

## 6. Factory reset
- [x] Non-blocking: button sampled on core 1 (`lib/power_monitor/reset_button`), reset done by core 0 — native `test_reset_button`
- [x] 10 s hold counted from the debounced press, triggers once, release just before 10 s cancels, button held at boot ignored — native
- [x] LED feedback: slow blink while held, fast after trigger — native
- [x] Defaults saved (incl. trap destination), reboot within 2 s of the trigger
- [x] Test with the real button (SW1 on the carrier): timing, LED, SNMP keeps answering, settings restored — HIL `test_factory_reset.py` (manual, `RFPOWER_MANUAL_BUTTON=1`), passed 2026-10-26

## 7. Testing infrastructure
- [x] `native` environment, MinGW-w64 on Windows, `-Wall -Wextra -Werror`
- [x] On-device Unity tests in `test/embedded`, auto-wait for the USB port after flashing
- [x] HIL tests in `test/hil` (Python, pyserial from PlatformIO)
- [x] Test rules in `.clinerules`, guide in `docs/TESTING.md`
- [x] CI (GitHub Actions): native tests, sanitizers, net-snmp check, firmware + test program build — dry run in a clean WSL copy
- [x] First CI run on GitHub (after push) — green since 2.5.1 (2026-10-31)

## 8. Security
- [x] Request rate limiting: 50/s per source, burst 100, 8 sources tracked (`lib/access_control`) — native + HIL
- [x] Global limit 100/s, burst 200, so rotating/spoofed sources cannot reset the per-source buckets (found by automated security review) — native `test_rotating_sources_cannot_exceed_global_limit`
- [x] Sources with the right community are trusted: own shared bucket and evicted last, so a spoofed flood cannot starve managers — native `test_trusted_manager_is_not_starved_by_spoofed_flood`, `test_spoofed_sources_cannot_evict_trusted_ones`
- [x] Trust bounded: trusted sources share 200/s (burst 400), at most 4 trusted, trust expires after 10 min idle, cleared when the community changes — native `test_trusted_sources_share_their_own_bounded_bucket`, `test_number_of_trusted_sources_is_capped`, `test_trust_expires_after_inactivity`, `test_clear_trust_revokes_every_trusted_source`; HIL checks the clear on community change
- [x] Expired trust is swept from the whole table on every request, so idle managers do not keep trust slots or eviction protection — native `test_idle_trusted_sources_release_their_slots`, `test_expired_trust_does_not_protect_against_eviction`
- [x] Rate limiter no longer freezes after an idle gap of more than 2^31 ms — native `test_idle_gap_longer_than_24_days_refills_instead_of_freezing`
- [x] Access logging: rejected requests with source, folded repeats, menu option 5 and serial log — native + HIL
- [x] authenticationFailure trap for wrong communities, throttled (see 2.6)
- [x] GetBulk replies capped at 16 varbinds against reflection/amplification with faked sources, and GetBulk does not grant trust — native `test_getbulk_response_is_capped_against_amplification`, `test_getbulk_outcome_does_not_count_as_trust`, net-snmp, HIL
- [x] Malformed packet tests for the BER layer (truncation, every single-byte corruption, sanitizers)
- [x] Same for the message and agent layers, plus malformed floods against the device (HIL)

## 9. Documentation
- [x] README: hardware, menu, storage, build, test
- [x] docs/TESTING.md
- [x] MIB document (docs/MIB.md) and MIB file
- [x] CHANGELOG.md (Keep a Changelog) and version tags (`v1.0.0`)
- [x] docs/LIBRENMS.md

## 10. Web page
- [x] HTTP request parser: bounded (2 KB), incremental, rejects chunked/oversized/malformed requests — native `test_web_ui` incl. every truncation and single-byte corruption, sanitizers
- [x] Login with user admin, random per-device default password (shown on serial only), own password via serial option 6 stored as salted iterated SHA-256 — native, HIL `test_own_password_from_serial_menu`
- [x] Sessions (HttpOnly, SameSite=Strict, 15 min idle, max 2), CSRF token per session, 30 s lockout after 5 failures, access log entries — native, HIL
- [x] Pages: status, DHCP/static IP (validated, save + restart), SNMP community (active immediately), restart, log out — native with fake backend, HIL incl. static IP round trip
- [x] Non-blocking server: slow or stuck clients never delay SNMP; 5 s connection timeout — HIL `test_slow_client_does_not_stall_snmp_and_is_dropped`, `test_page_load_burst_keeps_snmp_fast_and_stack_headroom`
- [x] Core 0 stack high-water mark on the status screen; HIL asserts > 1 KB headroom after web traffic
- [x] Own-password hash checked in steps (40 rounds per loop pass) so SNMP stays under 100 ms during a login — native `test_stepwise_check_*`, HIL `test_login_with_own_password_keeps_snmp_fast`
- [x] Session cookie found among up to 1 KB of other cookies of the same host — native `test_session_found_among_many_other_cookies`
- [x] Checked with real Chromium (headless, Playwright): login, reloads, form post, logout all under 120 ms
- [x] Serial option 7: replace the default password with a new random one (stored, read back, old one refused, browsers logged out) — HIL `test_new_default_password_from_serial_menu`, `test_new_default_keeps_an_own_password_in_use`
- [x] Bootstrap 5.3.3 styling, CSS embedded in flash (gzip, cached), works without internet — native, HIL `test_bootstrap_stylesheet_is_served_from_flash`, checked in Chromium (desktop and phone)
- [ ] HTTPS (decided 2026-10-17: plain HTTP on the management LAN)

## 13. IPv6 (2.3.0)
- [x] Dual stack (`PIO_FRAMEWORK_ARDUINO_ENABLE_IPV6`): link-local + SLAAC, SNMP/web/mDNS over IPv6, addresses on the status screen and web page — HIL `test_ipv6.py`
- [x] lwIP timer pool +12 (`MEMP_NUM_SYS_TIMEOUT`): with IPv6, mDNS emptied it and lwIP panicked; found with the `RFPOWER_DIAG` build
- [x] Back-to-back UDP packets crashed the IPv6 build (core `UdpContext` helper pbuf flags `0xFF` include `PBUF_FLAG_IS_CUSTOM`): `LWIP_IPV6_FRAG=0`, IPv6 replies <= 1452 bytes — HIL `test_back_to_back_requests_are_answered_and_do_not_crash`
- [ ] Report the `PBUF_HELPER_FLAG 0xff` bug to arduino-pico (UdpContext.h), then `LWIP_IPV6_FRAG` could go back on (still in arduino-pico master, 2026-10-31)
- [x] Platform pinned to commit `24f5f5b` (Arduino-Pico 5.4.3, lwIP 2.2.1, GCC 14.2) instead of the moving git HEAD (2026-10-31)
- [ ] Later: move the pin to Arduino-Pico 6.x (GCC 16, Pico SDK 2.3.0; 6.1.0 in the PlatformIO platform). No DHCPv6 addresses or UDP fix in it, so only when needed; re-measure the stacks and run every test layer
- [x] Each core gets its own 8 KB stack (`core1_separate_stack`): with IPv6 only ~660 bytes of core 0's 4 KB were left after a page-load burst; now ~5.3 KB stay unused — HIL `test_page_load_burst_keeps_snmp_fast_and_stack_headroom`
- [x] Rate limiter and access log keyed per full IPv4 or IPv6 address (`SourceKey`, 128 bits), ::/32 sources dropped; not per /64, so trust does not pass to other hosts in a manager's /64 — native `test_access_control`, HIL `test_wrong_community_over_ipv6_is_logged_with_the_full_pc_address`
- [x] Stage 2: IPv6 trap destination (v2c only; global or ULA, refused with v1) — native `test_netconfig` (`test_trap_target_*`), `test_settings_codec` (`test_ipv6_trap_target_*`, `test_trap_version_fits_target`), sanitizers; HIL `test_v2c_trap_reaches_an_ipv6_destination`, `test_menu_refuses_*` (passed 2026-10-31; a VPN such as Cisco AnyConnect blocks IPv6 to the device)
- [x] Static IPv6 address (/64) and gateway, serial menu option 9 and web page, next to SLAAC (2.4.0) — native `test_netconfig` (parse/format/validate, truncation), `test_settings_codec`, `test_web_ui`; HIL `test_ipv6_static.py` (opt-in `RFPOWER_STATIC_IPV6`)
- [x] DHCPv6 address assignment: stateful client, one IA_NA (`lib/dhcp6_client`, `src/dhcp6_service.cpp`), HIL tested on the test network in `test/hil/dhcp6/`
- [x] Hardware watchdog in normal builds (8 s; core 0 feeds only while core 1's heartbeat moves; restart cause and count on the status screen) — native `test_watchdog_logic`, sanitizers, HIL `test_watchdog.py` (core 0 and core 1 stalls restart the device, menu reboot not counted; passed 2026-10-31)
- [x] `SO_REUSE=1` instead of dropping TIME-WAIT entries on port 80 — HIL `test_web_reopens_at_once_after_the_device_closed_connections` (fails without either, passes with `SO_REUSE`), web/SNMP/serial menu/IPv6 suites and on-device tests pass
- [ ] ~~`DHCP_ADD_EXTRA_REQUEST_OPTIONS` + `LWIP_HOOK_DHCP_PARSE_OPTION` instead of editing DHCP frames~~ — dropped (2026-11-01): the frame editing works and is tested with real frames, and the checks that take option 224 only from an ACK lwIP accepted would stay, so little code goes away while a security-relevant path changes

## 12. Web on/off, DHCP trap destination, logo (2.2.0)
- [x] Web interface on/off: setting `webEnabled`, serial option 8, "Turn off…" on the page with confirmation; port 80 closed, sessions ended, mDNS `_http` removed — native, HIL
- [x] Trap destination `dhcp`: option 224 requested in the device's DHCP messages (frame edit at the driver) and read from ACKs; verified against Kea 2.2.0 in Docker and the LAN server — native `test_dhcp_option`, HIL `test_dhcp_trap.py`
- [ ] Opt-in HIL `test_trap_destination_comes_from_option_224` against the home DHCP server once option 224 is configured there (`RFPOWER_DHCP_TRAP_TARGET`)
- [x] Logo in the navbar, on the login card and as favicon — native, HIL

## 11. Hostname
- [x] One hostname = SNMP sysName, DHCP hostname (option 12) and mDNS `<name>.local` with `_http._tcp` — native `test_netconfig` hostname tests, HIL `test_hostname_from_web_applies_to_snmp_mdns_and_status`
- [x] Set in serial option 4 and on the web page, validated, active immediately (DHCP renew, mDNS rename)
- [x] Free-text sysName from older firmware keeps loading; a cleaned-up name is used for DHCP/mDNS
- [ ] Check that the DHCP server really shows the name (needs access to the router's lease list)
- [ ] SNMP latency spikes: on 2.0.1/2.1.0 about 3 % of requests took 50-97 ms (median 3.7 ms); on 2.3.0 (8 KB stacks, IPv6) 600 requests: median 3.8 ms, max 42.5 ms, none over 50 ms. One 116 ms sample in a full HIL run (not reproduced in 3 reruns); keep watching

## 14. Hardware: carrier board (`hardware/pico-poe-mains-carrier`)
- [x] Carrier PCB for the W5500-EVB-Pico-PoE: isolated mains detector on GP27, LEDs, TMP1075; KiCad 10, ERC/DRC, fab files (documented in the README)
- [x] First boards ordered and assembled; mains loss/restore on the carrier seen on the serial menu — HIL `test_mains_loss_and_restore_are_reported` (Dirigera, passed 2026-10-31)
- [x] SNMP and trap HIL tests on the carrier incl. real mains (`test_snmp.py` 41 tests, `test_snmp_traps.py` v1 and v2c; community `public`, LibreNMS snmptrapd stopped while they ran) — passed 2026-10-31
- [x] Read the TMP1075 (I2C0, GP4/GP5, 0x48) in the firmware: `ambientTemperature` (63050.5), absent without the sensor; the RP2040 sensor is now `cpuTemperature` — native, net-snmp, HIL
- [x] Factory reset button: SW1 (XKB TS-1187A) from GP22 to GND on the carrier, board v1.1; ERC/DRC clean. Tested with the real button (HIL `test_factory_reset.py`)
- [x] Fuse in the mains input: F1 SETfuse SCT1032T1A250V (LCSC C19712559), T 1 A, 250 VAC, in the single series loop; ERC/DRC clean
- [x] Mains loss timing: the carrier's RC delays the loss by about 30-50 ms; requirement changed to 80 ms from mains to detection, firmware share still 10 ms (decided 2026-10-31, `.clinerules`)
- [ ] Optional: confirm the RC delay with a scope (last optocoupler pulse to SENSE rising) on an assembled board

## 15. LLDP (2.6.0)
- [x] Send LLDPDUs (chassis/port ID MAC, TTL 120, port description, system name/description, station only, IPv4/IPv6 management addresses) every 30 s, 1 s after link up and after a change — native `test_lldp` byte-exact (bytes checked with tcpdump -vv), HIL `test_switch_lists_the_device`, `test_switch_learns_new_hostname_within_5_s` (fails with a firmware that ignores changes) against a Juniper EX2200-C
- [x] Read the switch's LLDPDUs, keep one neighbour (TTL, TTL 0, link down), show switch and port on the status screen and web page — native `test_lldp` incl. a captured EX2200-C frame, every truncation, sanitizers; `test_web_ui` escaping and longest page; HIL `test_status_shows_the_switch`
- [x] LLDP counted as received multicast, not unknown protocol — native `test_if_stats`
- [x] LLDP-MIB over SNMP (2.7.0): `lldpLocPortTable` and `lldpRemTable` (one row, Junos port name as Port ID), so LibreNMS shows the switch and port on the device's Neighbours tab without monitoring the switch — native `test_lldp`, `test_snmp_agent` (v1/v2c, walks with and without a neighbour, noSuchInstance), `host_agent_check.sh` against net-snmp, HIL `test_snmp_lldp_tables_name_the_switch`, `test_snmp.py` walks; LibreNMS discovery: link eth0 → r169 / ge-0/0/9.0 (`autodiscovery.xdp` off)
- [ ] lldpRemManAddrTable (the switch's management address): not served, the EX2200-C sends none

## Notes
- SNMP must be v1 and v2c (no v3) and our own code; networking may use libraries
- Memory limits: 70 % flash, 50 % RAM (2.3.0: 14.2 % / 41 %, built with -Os)
- Version: `include/version.h`; every user-visible change goes into CHANGELOG.md under [Unreleased]
- Core 0 has an 8 KB stack (core1_separate_stack, since 2.3.0) shared with USB/lwIP/W5500 interrupts: keep big buffers static
