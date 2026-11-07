# RF-Power-Monitor TODO

An item is only ticked when it is implemented **and** its tests exist and pass (see `.clinerules`, Test Rules).

## 1. Network
- [x] W5500 bring-up via the core's lwIP driver (`Wiznet5500lwIP`), hardware reset on GPIO20
- [x] DHCP client (lwIP), lease renewal — `test/embedded/test_ethernet`, HIL
- [x] Static IP from settings — HIL `test_static_ip_applies_after_reboot` (passed with 10.10.6.249, 2026-10-16)
- [x] Boot without a serial terminal attached (3 s wait max)
- [x] No lease: keep retrying DHCP, no fallback address (decided 2026-10-16, avoids IP conflicts)
- [x] Boot does not wait for DHCP, menu available immediately — HIL `test_menu_is_available_before_dhcp_lease`
- [x] Status shows connection state (connected / waiting for lease / no link / no W5500) — HIL
- [x] Test the "no link" and "no DHCP server" states: the device's switch port disabled, or DHCP dropped by a port filter, over the switch console (`commit confirmed`); both recover without a reboot — HIL `test_switch_port.py` (opt-in `RFPOWER_SWITCH_CONFIG`, passed 2026-11-02 on the EX2200-C)
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
- [x] v3 (and unknown versions), response/trap PDUs and malformed packets reported separately
- [x] Build GetResponse; error responses echo the request varbinds (RFC 1157)
- [x] Byte-exact against net-snmp requests, every truncation and single-byte corruption
### 2.3 Operations — `lib/snmp_agent/snmp_agent`, `test/native/test_snmp_agent`
- [x] Community check (constant time), wrong community silently dropped and counted
- [x] GetRequest, noSuchName with error-index for unknown or non-leaf OIDs
- [x] GetNextRequest, lexicographic order, noSuchName past the end (net-snmp: "End of MIB")
- [x] SetRequest answered with noSuchName (everything read-only)
- [x] tooBig when the response does not fit; statistics counters
### 2.4 MIB — `lib/snmp_agent/snmp_mib`, `mibs/RF-POWER-MONITOR-MIB.txt`, `docs/MIB.md`
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
- [x] Trap destination setting (optional field, IPv4, IPv6 or empty) — native

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
- [x] Request rate limiting: 50/s per source, burst 100 (200/s, burst 400 for a trusted source, 3.1.0), 8 sources tracked (`lib/access_control`) — native + HIL
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
- [x] Own-password hash checked in steps (20 rounds per loop pass) so SNMP stays under 100 ms during a login — native `test_stepwise_check_*`, HIL `test_login_with_own_password_keeps_snmp_fast`
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
- [x] SNMP latency spikes: on 2.0.1/2.1.0 about 3 % of requests took 50-97 ms (median 3.7 ms); on 2.3.0 (8 KB stacks, IPv6) 600 requests: median 3.8 ms, max 42.5 ms, none over 50 ms. One 116 ms sample in a full HIL run (not reproduced in 3 reruns). On 3.5.1 (2026-11-07): 3000 requests at 20/s, median 3.2 ms, p99.9 5.1 ms, max 14.1 ms, none lost; 1500 more during five forced update-site check-ins, max 14.5 ms. Not reproduced: closed, `test_response_time_under_100ms` keeps watching

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

## 16. Name RF-Power-Monitor (3.0.0)
- [x] Renamed from RF-UPS-Monitor: firmware texts, default hostname `rf-power-monitor`, `RF-POWER-MONITOR-MIB`, LibreNMS OS `rfpowermonitor`, `RFPOWER_*` test variables, board generator, docs and diagrams — native suites (names pinned: default and fallback hostname, page title, stored old name kept), net-snmp check, HIL suites on the renamed firmware, LibreNMS rediscovery
- [x] Rename the GitHub repository: `TheKingOfChaos/RF-Power-Monitor` (2026-11-02; GitHub redirects the old URLs), links in CHANGELOG, `library.json` and the MIB updated
- [ ] Rename the local folder (the user's call)

## 17. Standard MIBs (3.1.0, plan docs/superpowers/plans/2026-11-02-standard-mibs.md)
- [x] sysContact and sysLocation on the web page (Device name card) — native `test_device_name_*`, HIL `test_contact_and_location_from_web_reach_snmp`
- [x] SNMPv2-MIB complete: sysORTable, snmp group, snmpSetSerialNo — native `test_sysortable_*`/`test_snmp_*`, net-snmp, HIL `test_snmp_group_counts_this_test_requests`, `test_sysortable_names_the_mibs`
- [x] HOST-RESOURCES-MIB: CPU load per core, RAM, LittleFS; ENTITY-MIB inventory — native `test_cpu_load`, `test_hr_*`, `test_entity_*`, net-snmp, HIL `test_host_resources.py`
- [x] IP/ICMP/UDP/TCP statistics from lwIP's MIB-II counters — native `test_ip_*`/`test_icmp_*`/`test_tcp_*`/`test_udp_*`, net-snmp, HIL `test_icmp_echo_counters_follow_ping`, `test_udp_*`, `test_tcp_*`
- [x] IP-MIB ipAddrTable and ipAddressTable (IPv4 and IPv6) — native `test_ip_tables_*`/`test_ipaddress*`/`test_dynamic_rows_*`, net-snmp, HIL `test_snmp_ip_addresses_match_status_screen`, `test_snmp_ipv6_addresses_match_status_screen`
- [x] LibreNMS shows location, CPU, memory, storage, addresses, inventory, netstats — LibreNMS 26.10.2 rediscovery 2026-11-02 (docs/LIBRENMS.md)

## 18. Default hostname from the MAC (3.2.0)
- [x] Default hostname `RF-Power-<last 6 MAC hex digits>`; empty sysName or the old default `rf-power-monitor` count as not set; `-` in menu option 4 goes back to it — native `test_default_hostname_*`, `test_unset_hostname_*`, `test_chosen_hostname_is_kept`, settings default empty; HIL `test_default_hostname_is_rf_power_and_the_mac_tail`
- [x] Hostname changes before a DHCP lease no longer call `dhcp_renew()` (it stopped DHCP from getting a lease) — HIL: device gets its lease with the MAC-based name

## 19. Hardware: rev 2 board (`hardware/rf-power-monitor-rev2`)
- [x] Design: RP2040 + W5500 on the board, WIZPoE-P1 plug-in PoE module, rev 1 mains detector, TMP1075 and factory reset; same pin map. KiCad 10, 2 layers, 100 x 64 mm; ERC 0, DRC 0 errors (2 silkscreen warnings); JLCPCB Gerbers, BOM and CPL (designed with Adom AI Flow, 2026-11-03)
- [ ] Human review of schematic, board, BOM and fab files before ordering
- [ ] Order and assemble; bring-up measurements from `design/tests.json` (rails, crystals, W5500, mains sense, LEDs)
- [ ] Check that the WIZPoE-P1 does not cycle at the board's ~134 mA load (module minimum 150 mA); add a bleed load if it does
- [ ] Firmware on rev 2: flash over USB-C, then the full native, embedded and HIL suites, incl. mains loss/restore and the factory reset button
- [ ] `HARDWARE_REVISION` (entPhysicalHardwareRev) for rev 2 boards: the firmware cannot tell the boards apart, so decide on a build flag or a stored setting
- [ ] Datasheet's pin table lists GP29 as VSYS/3, but GP29 is not connected on rev 2: fix it when the datasheet is regenerated
- [ ] New case (the rev 1 case does not fit)

## 20. Firmware update (A/B slots, signed)
- [x] Flash layout: boot program, slot A, slot B, LittleFS and EEPROM where they were; firmware linked per slot (`scripts/slot_build.py`), `firmware_full.uf2` for USB, `default_envs = pico` — tool tests (`test/tools`), `pio run -e pico`, flashed and booted on the Pico
- [x] Boot program: verifies the Ed25519 signature of a slot, picks it from the two-sector boot record, falls back after 3 crashes, LED pattern when nothing boots — native `test_fw_boot`, `test_fw_records`, `test_fw_image` (RFC 8032 vectors, byte-compatible with `tools/fwsign`), embedded `test_fw_flash`, HIL `BootProgramTest`
- [x] Signed `.upd` upload over the web page and `POST /firmware/upload`, streaming into the other slot, key check, rejected uploads leave it empty — native `test_fw_receiver` (golden `.upd`, truncated and corrupted input, sanitizers), `test_web_ui`, embedded `test_fw_flash`, HIL `FirmwareUpdateTest`
- [x] Trial boot, rollback (crash, unreachable, 60 min limit), fallback after 3 crashes of a kept firmware, manual switch — native `test_fw_trial`, `test_fw_boot`, `test_fw_records`, HIL (crash, shorttrial, crash loop, switch; PoE cycles opt-in `RFPOWER_POE_CYCLE=1`)
- [x] SNMP `fwUpdate` group (63050.7), traps `firmwareUpdateKept` and `firmwareRolledBack`, LibreNMS sensors — native `test_snmp_agent`, `test_snmp_trap`, net-snmp `host_agent_check.sh`, HIL, LibreNMS
- [x] `tools/fw_keygen.py`, `fw_release.py`, `fw_update.py`, serial status lines, user and setup manuals — tool tests, native, HIL
- [x] Final verification of 3.3.0: all native suites, sanitizers, net-snmp check, tool tests, on-device suites, full HIL and the opt-in firmware update set (see the task report and CHANGELOG)
- [x] Boot verification time (3.5.1): own SHA-512 for the M0+ (`lib/fw_image` `fw_sha512`), 921 ms instead of 1080 ms for 430 KB; the 1.5 s budget lasts to ~740 KB (was ~570 KB), `scripts/slot_build.py` warns from 700 KB — native `test_fw_image` (FIPS vectors, every length 0-400 and splits against Monocypher), embedded `test_fw_sha512` (M0+ build against Monocypher, speed), tool tests, HIL `BootProgramTest`
- [ ] Later, if the image nears 700 KB: SHA-512 compression in Thumb-1 assembly (estimated ~2x on the hash; decided 2026-11-07 to keep the C version until then). Running the hash from RAM and `-O2` were measured and gain nothing (1080 -> 1074 ms, -O2 slower with the unrolled code)
- [x] Refused uploads without a login (401/403) drain the body within 20 s, no pause over 5 s, instead of the 5-minute upload deadline (3.5.1) — native `test_web_ui` (`StreamBody::drainOnly`), HIL `test_trickled_upload_without_login_is_dropped_in_seconds`

## 21. Update site (3.4.0, plan docs/superpowers/plans/2026-11-06-update-server.md)
- [x] Check-in wire format and keyed BLAKE2b MACs, device and site against shared vectors — native `test_update_protocol`, `update-server/tests/test_protocol.py`, sanitizers
- [x] URL and HTTP response parsing, `.upd` header version, check-in timing, update settings codec, download decisions — native `test_update_client`, sanitizers
- [x] DHCP option 225 (text) beside 224 — native `test_dhcp_option`
- [x] Update settings in their own A/B files, cleared by a factory reset — embedded `test_settings_store`
- [x] Non-blocking HTTP client on lwIP (2-segment window), update service, trial confirmation by a check-in — HIL `test_update_server.py` (opt-in `RFPOWER_UPDATE_SERVER_TEST=1`), device check with `fetchtest`
- [x] Serial menu option 10, status line, Firmware page card — native `test_web_ui`, HIL `test_serial_menu.py`
- [x] Site: store, assignment rules, check-in API, pages, login, Docker — `update-server/tests` (CI), container checked
- [ ] Site over HTTPS (behind a reverse proxy) if check-ins must not be readable on the network
- [x] SNMP objects for the update server state: `updateSite` group (63050.8, 3.6.0), see section 23

## 22. Bench setup and device label (3.5.0, plan docs/superpowers/plans/2026-11-06-bench-provisioning.md)
- [x] Default web password kept twice, used copy chosen and the other repaired at boot; loss while locked reported — `test/native/test_web_ui`
- [x] `pwlock`/`pwunlock`, option 7 refused while locked, lock survives reboot — HIL `test_bench.py`
- [x] `benchinfo` block — `test/native/test_bench_info`, HIL `test_bench.py`
- [ ] Lock survives a factory reset — manual HIL `test_factory_reset.py` (needs the button pressed)
- [x] Bench tool: release UF2 check, label, log, flash/no-flash/reprint — `test/tools/test_bench_*.py`; tried with `--no-flash` on the test device (2026-11-07)
- [x] Update server default from `secret/bench.ini`, restored by a factory reset — `test/embedded/test_settings_store`, HIL `test_bench.py`, `test/tools/test_bench_*.py`; tried with `--no-flash` on the test device (2026-11-07)
- [x] Review minors: signature check of the release, log writable before flashing, one new port at a time, hostname check, older-firmware option 7 detected, HIL tests restore option 10 and the lock
- [ ] First real rev 2 board through the bench tool, label printed on the NIIMBOT M2
- [ ] Print straight from the script to the M2 (once the printer is here; NiimBlue does not list the M2_H as tested)

## 23. Time and syslog (3.6.0, plan docs/superpowers/plans/2026-11-07-time-syslog.md)
- [x] Clock: civil time from epoch, Danish local time (CET/CEST, EU rule), RFC 5424 UTC stamp, SNMP DateAndTime — native `test_clock`, sanitizers
- [x] NTP and syslog settings in their own A/B files (`data/timelog_a.json`/`_b.json`), `dhcp` by default and after a factory reset, main settings file untouched (rollback to 3.5.1 keeps its settings) — native `test_syslog` (codec, truncated JSON), embedded `test_settings_store`
- [x] Syslog messages (RFC 5424, daemon, `rfpower`), queue of 16 (oldest dropped and counted), access events throttled to 5 a minute, events dated from when they happened — native `test_syslog`, sanitizers
- [x] DHCP option 7 (syslog server): asked for, first address read from our ACKs — native `test_dhcp_option` (several addresses, bad lengths, truncation and corruption)
- [x] NTP through lwIP's SNTP client (DHCP option 42, DHCPv6 option 56, typed address or name), every 15 minutes; hrSystemDate once synced — native `test_snmp_agent` (`test_hr_system_date_*`), HIL `test_time_syslog.py` `TimeTest` (opt-in `RFPOWER_NTP_SERVER`)
- [x] Syslog events: MAINS, BOOT, FIRMWARE, WEB, SNMP (access log), SETTINGS (no secrets), NETWORK, LLDP, UPDATE; queue sent before a planned restart; undated with `[uptime N s]` after 2 minutes without the time — HIL `test_time_syslog.py` `SyslogTest` (opt-in `RFPOWER_SYSLOG_TEST=1`; settings change without the community, throttled burst, dated BOOT line, mains loss with the mains switch)
- [x] Serial menu option 11, `Time:` and `Syslog:` status lines, local time in the access log — HIL `test_serial_menu.py` (`test_option_11_*`, `test_access_log_lines_still_parse`)
- [x] Web page: Time and Syslog rows, card *Time and syslog* (`/timelog`) — native `test_web_ui` (`test_timelog_*`), HIL `test_time_and_syslog_from_web_page_apply`
- [x] SNMP `updateSite` group (63050.8: updCheckinResult, updMinutesSinceGood, updFailedInRow), LibreNMS sensors "Update site check-in" and "Minutes since update site check-in" (warning above 60) — native `test_snmp_agent` (`test_update_site_group_*`), net-snmp `host_agent_check.sh`, HIL `test_snmp.py` walks
- [x] LibreNMS syslog receiver in the Docker test setup: `syslogng` sidecar, RFC 5424 source on UDP 5140 (`librenms/test/syslog-ng-rfc5424.conf`), `enable_syslog` — device messages on its Logs → Syslog tab (2026-11-07, docs/LIBRENMS.md)
- [ ] DHCPv6 option 56 (NTP server) only through lwIP's stateless DHCPv6, i.e. when the router advertisement sets the O flag; a network with only the M flag gives no NTP server over DHCPv6

## Notes
- SNMP must be v1 and v2c (no v3) and our own code; networking may use libraries
- RAM after 3.4.0: static 49.6 % (130,040 bytes), about 1 KB below the 50 % limit then. The limit was raised to 60 % on 2026-11-07 (about 27 KB of room) for SNTP and syslog; static RAM with them (3.6.0): 52.8 %
- Memory limits: 70 % flash, 60 % RAM (50 % until 2026-11-07; 3.3.0: image 44 % of a slot, 21.3 % of the core's flash figure; static RAM 48.8 %, measured as PlatformIO's RAM line (.data + .bss); an upload allocates nothing more; built with -Os). About 3 KB of RAM is left below the 50 % limit: check the build's RAM line before adding buffers (the web page body and response alone are 22 KB)
- Version: `include/version.h`; every user-visible change goes into CHANGELOG.md under [Unreleased]
- Core 0 has an 8 KB stack (core1_separate_stack, since 2.3.0) shared with USB/lwIP/W5500 interrupts: keep big buffers static
