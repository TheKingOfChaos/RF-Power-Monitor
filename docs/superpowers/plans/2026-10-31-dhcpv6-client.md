# DHCPv6 Client Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The device takes one IPv6 address from a DHCPv6 server (stateful, IA_NA) when the router's advertisements carry the M flag, with an end-to-end test on a test network that does not disturb the LAN.

**Architecture:** A pure library `lib/dhcp6_client` (message codec, frame wrap/unwrap and router-advertisement reading, client state machine with RFC 8415 timers) is driven by a thin firmware layer `src/dhcp6_service.cpp`. Frames are read in the existing netif input hook and sent as complete Ethernet frames through the link output (`src/net_stats.cpp`), like DHCP option 224, so lwIP's stateless DHCPv6 keeps UDP 546. The test network is a unicast-only router advertisement plus a Kea server that answers only the device, run from this PC by the HIL test.

**Tech Stack:** C++17 (pure libs, Unity native tests, MinGW and WSL sanitizers), Arduino-Pico 5.4.3 / lwIP 2.2.1 (pinned), Python 3 HIL tests, Kea DHCPv6 (ISC), WSL Ubuntu (mirrored networking), scapy for the router advertisement.

**Spec:** `docs/superpowers/specs/2026-10-31-dhcpv6-client-design.md`

## Global Constraints

- One IA_NA address; no IA_PD, no IA_TA, no Reconfigure, no authentication, no rapid commit, no setting.
- Client starts only on a router advertisement with the M flag; nothing changes on networks without it (all existing tests pass unchanged).
- DUID-LL (type 3, hardware type 1) from the MAC; IAID 1; hostname (`hostnameCurrent()`) in Client FQDN (option 39).
- Retransmission per RFC 8415 section 7.6/15: SOL_MAX_DELAY 1 s, SOL_TIMEOUT 1 s, SOL_MAX_RT 3600 s, REQ_TIMEOUT 1 s, REQ_MAX_RT 30 s, REQ_MAX_RC 10, REN_TIMEOUT 10 s, REN_MAX_RT 600 s, REB_TIMEOUT 10 s, REB_MAX_RT 600 s, DEC_TIMEOUT 1 s, DEC_MAX_RC 4, RAND ±0.1.
- Address added only after lwIP's DAD; Decline on duplicate; removed at the end of its valid lifetime; never evicts another address; `LWIP_IPV6_NUM_ADDRESSES` stays 4.
- Core 0 never blocks; no stack buffer over ~256 bytes (static instead); SNMP stays under 100 ms.
- Every received byte is untrusted: bounds-checked parsing, truncation and single-byte-corruption tests, `scripts/sanitize.sh` clean.
- Protocol tests use bytes captured from real tools (Kea, a Linux DHCPv6 client, the device's own frames).
- Test network: router advertisements only unicast to the device's link-local address, M (or O) flag, router lifetime 0, no prefix information; Kea answers only the device's DUID; both run only during the test; opt-in env var `RFUPS_DHCP6_TEST=1`. Never an RA with M to the LAN multicast address.
- The TODO item is ticked only when the HIL test passes on the test network.
- Native code builds with `-Wall -Wextra -Werror`; tests named `test_<what>_<expected behaviour>`; all functions documented; CHANGELOG under [Unreleased].

## Review Focus

- A Reply/Advertise for another client or another exchange (wrong transaction id, other client DUID, missing server id) must be ignored, never applied. Test: `test_parse_rejects_reply_for_other_client` (Task 3), `test_client_ignores_reply_with_wrong_xid` (Task 5).
- A server lease with preferred > valid, valid 0, or an IA status other than Success must not install an address. Test: `test_parse_drops_address_with_preferred_above_valid` (Task 3), `test_client_returns_to_solicit_on_noaddrsavail` (Task 5).
- No server at all on an M network: retransmissions back off to SOL_MAX_RT and never exceed it, also across `millis()` wrap. Test: `test_solicit_backs_off_to_max_rt_and_survives_millis_wrap` (Task 5).
- All four address slots taken: no eviction, a visible reason. Test: `test_pick_slot_returns_none_when_all_used` (Task 5) and the status line in Task 7.
- Network without the M flag (every LAN today): the client never sends anything. Test: `test_client_sends_nothing_before_start` (Task 5) and the existing HIL suites in Task 8.

---

### Task 1: Test-network spike (no firmware changes)

Proves, before any client code exists, that this PC can (a) deliver a unicast router advertisement to the device and (b) receive the device's multicast DHCPv6 messages in Kea and answer them. Uses the current firmware: an RA with only the **O** flag makes lwIP's existing stateless client send an Information-request to ff02::1:2, which Kea must answer.

**Files:**
- Create: `test/hil/dhcp6_testnet.py` (start/stop the RA sender and Kea; helpers below)
- Create: `test/hil/dhcp6/kea-dhcp6.conf.in` (template: one subnet = the device's /64, short lifetimes, a host reservation for the device's DUID, `"client-classes"` so unknown clients get nothing)
- Create: `test/hil/test_dhcp6_testnet.py` (the spike as a test, kept as a regression for the test network)

**Interfaces:**
- Produces (Python, `dhcp6_testnet.py`):
  - `class TestNet: __init__(self, device_mac: str, device_link_local: str, interface: str)`; `start(self, managed: bool, other: bool, valid_s: int = 120, preferred_s: int = 60, pool_address: str = None)`; `stop(self)`; `kea_log(self) -> str`; context manager (`with TestNet(...) as net:` always stops).
  - `device_duid_ll(mac: str) -> str` (hex, `00:03:00:01:` + MAC).
  - RA sender: scapy in WSL (`sudo python3 -c ...` or a script under `test/hil/dhcp6/send_ra.py`), `IPv6(dst=<device link-local>, hlim=255)/ICMPv6ND_RA(M=..., O=..., routerlifetime=0)` with no prefix option, resent every 3 s while running.
  - Kea: `kea-dhcp6` in WSL (apt `kea-dhcp6-server`, or the ISC Docker image with host networking in WSL; pick what the spike shows works), listening on the WSL interface that mirrors the LAN.

- [ ] **Step 1: Check prerequisites and record them**

Run in WSL: `ip -6 addr`, `cat /mnt/c/Users/$USER/.wslconfig` (expect `networkingMode=mirrored`), `python3 -c "import scapy"`, `which kea-dhcp6`.
Install what is missing (`sudo apt install python3-scapy kea-dhcp6-server`). If UDP 547 inbound is blocked by the Hyper-V firewall (as UDP 162 was), stop and ask the user to add a rule `RFUPS-DHCPv6` (UDP 547, WSL) as administrator; do not change firewall settings yourself.

- [ ] **Step 2: Write `test_dhcp6_testnet.py`**

`test_kea_answers_the_devices_information_request` (skips unless `RFUPS_DHCP6_TEST=1`): read MAC and link-local address from the status screen (`Device.status()["raw"]`, `IPv6: FE80::...`), `with TestNet(mac, ll, iface) as net: net.start(managed=False, other=True)`, wait up to 20 s for `net.kea_log()` to contain an INFORMATION-REQUEST from the device's DUID and a REPLY sent. Assert both. Also assert the device's status afterwards still shows its SLAAC address and gateway unchanged (router lifetime 0, no prefix).

- [ ] **Step 3: Run it and iterate on the mechanics until it passes**

Run: `$env:RFUPS_DHCP6_TEST="1"; python.exe -m unittest discover -s test/hil -p "test_dhcp6_testnet.py" -v`
Expected: PASS. If (a) or (b) cannot work from WSL mirrored mode or Docker, stop and report to the user with what was tried; the spec's fallback (isolated link with a spare Ethernet port) needs their hardware.

- [ ] **Step 4: Capture the device's Information-request and Kea's Reply** (tcpdump in WSL, `-w test/hil/dhcp6/spike.pcap`) for Task 2.

- [ ] **Step 5: Commit**

```bash
git add test/hil/dhcp6_testnet.py test/hil/dhcp6/ test/hil/test_dhcp6_testnet.py
git commit -m "HIL: DHCPv6 test network (unicast RA, Kea for the device only)"
```

### Task 2: Captured fixtures

**Files:**
- Create: `scripts/dhcp6_capture/compose.yml` (Kea DHCPv6 + a Linux client container on a Docker bridge, tcpdump), `scripts/dhcp6_capture/README.md` (how to regenerate)
- Create: `test/native/dhcp6_frames/frames.h` (shared C arrays, one comment per frame saying where it came from)

**Interfaces:**
- Produces in `frames.h`: `KEA_ADVERTISE[]`, `KEA_REPLY_REQUEST[]`, `KEA_REPLY_RENEW[]`, `KEA_ADVERTISE_NOADDRSAVAIL[]`, `CLIENT_SOLICIT[]` (from the Linux client, for comparison), `DEVICE_INFO_REQUEST[]` and `KEA_INFO_REPLY[]` (Task 1 capture), `ROUTER_RA_LAN[]` (the LAN router's RA as the device receives it), `TESTNET_RA_M[]` (the Task 1 sender with M set), each with `_LEN`; plus the MACs/addresses they belong to. Full Ethernet frames.

- [ ] **Step 1:** Write the compose file: Kea pinned version, a client (`odhcp6c` or `dhclient -6`) with a fixed MAC, tcpdump writing a pcap. Run it, extract the frames above (a short Python script with scapy, also under `scripts/dhcp6_capture/`), write `frames.h`.
- [ ] **Step 2:** Also run the client against Kea with `NoAddrsAvail` (pool exhausted) for `KEA_ADVERTISE_NOADDRSAVAIL`.
- [ ] **Step 3: Commit** `git commit -m "DHCPv6 fixtures captured from Kea, a Linux client and the device"`

### Task 3: Message codec

**Files:**
- Create: `lib/dhcp6_client/library.json` (as `lib/watchdog_logic/library.json`), `lib/dhcp6_client/include/dhcp6_message.h`, `lib/dhcp6_client/src/dhcp6_message.cpp`
- Test: `test/native/test_dhcp6_message/test_main.cpp` (includes `../dhcp6_frames/frames.h`)
- Modify: `scripts/sanitize.sh` (add `[test_dhcp6_message]="dhcp6_client"`, as for the other suites)

**Interfaces:**
- Produces (`dhcp6_message.h`):
  - Message types `DHCP6_SOLICIT=1, DHCP6_ADVERTISE=2, DHCP6_REQUEST=3, DHCP6_RENEW=5, DHCP6_REBIND=6, DHCP6_REPLY=7, DHCP6_DECLINE=9`; `DHCP6_SERVER_ID_MAX = 130`; `DHCP6_STATUS_SUCCESS=0, DHCP6_STATUS_NOADDRSAVAIL=2, DHCP6_STATUS_NOBINDING=3, DHCP6_STATUS_NOTONLINK=4`.
  - `struct Dhcp6Lease { uint8_t addr[16]; uint32_t preferred; uint32_t valid; uint32_t t1; uint32_t t2; };`
  - `struct Dhcp6Out { uint8_t type; uint32_t xid; uint16_t elapsedCs; const uint8_t* serverId; uint8_t serverIdLen; const uint8_t* addr; const char* hostname; };` (`serverId`/`addr` nullptr when absent)
  - `size_t dhcp6Build(const Dhcp6Out& m, const uint8_t mac[6], uint8_t* out, size_t size);` returns 0 if it does not fit. Options: Client ID (DUID-LL), Server ID (Request/Renew/Decline), IA_NA (IAID 1, T1/T2 0, IA Address with lifetimes 0 when `addr`), Elapsed Time, ORO {82} (Solicit), Client FQDN flags S=1 with `hostname` as one DNS label (Solicit/Request/Renew/Rebind).
  - `struct Dhcp6In { uint8_t type; uint32_t xid; uint8_t serverId[DHCP6_SERVER_ID_MAX]; uint8_t serverIdLen; uint8_t preference; uint16_t status; uint16_t iaStatus; bool hasLease; Dhcp6Lease lease; };`
  - `enum class Dhcp6Parse : uint8_t { Ok, Malformed, NotForUs, NotServerMessage };`
  - `Dhcp6Parse dhcp6Parse(const uint8_t* msg, size_t len, const uint8_t mac[6], Dhcp6In& out);` accepts Advertise and Reply only; requires Client ID = our DUID-LL and a Server ID; IAID must be 1; an IA Address with preferred > valid or valid 0 is dropped (`hasLease` false); `out` untouched unless `Ok`.

- [ ] **Step 1: Write failing tests**
  - `test_build_solicit_matches_rfc_layout`: decode the built Solicit field by field: type 1, xid 3 bytes, Client ID option 1 = `00 03 00 01` + MAC, IA_NA option 3 with IAID 1, Elapsed Time option 8, ORO option 6 containing 82, FQDN option 39 = `01` + `0e "rf-ups-monitor" 00`.
  - `test_build_request_carries_server_id_and_address`, `test_build_decline_has_address_and_no_fqdn`, `test_build_returns_zero_when_buffer_too_small`.
  - `test_parse_kea_advertise` (address, lifetimes, preference, server id from `KEA_ADVERTISE`), `test_parse_kea_reply`, `test_parse_noaddrsavail_has_no_lease_and_status_2`.
  - `test_parse_rejects_reply_for_other_client` (Kea's reply checked against another MAC → `NotForUs`), `test_parse_rejects_client_messages` (`CLIENT_SOLICIT` → `NotServerMessage`), `test_parse_drops_address_with_preferred_above_valid` (fixture copy with lifetimes edited), `test_parse_drops_address_with_valid_zero`.
  - `test_parse_survives_every_truncation` and `test_parse_survives_every_single_byte_corruption` (all fixtures; must not crash; the sanitizers check reads), `test_parse_leaves_output_untouched_on_failure`.
  - `test_kea_accepts_the_built_solicit`: not native; done in Task 2's compose by replaying `dhcp6Build` output (dump from a tiny host program) and checking Kea answers with an Advertise. Record the result in the README.
- [ ] **Step 2:** Run `pio test -e native -f native/test_dhcp6_message`; expected FAIL (missing symbols or stub).
- [ ] **Step 3:** Implement `dhcp6Build` and `dhcp6Parse` in `dhcp6_message.cpp` (bounds-checked TLV walk; nested IA_NA options; big-endian helpers local to the file).
- [ ] **Step 4:** Run native suite and `wsl -d Ubuntu -- bash scripts/sanitize.sh`; expected PASS.
- [ ] **Step 5: Commit** `git commit -m "dhcp6_client: DHCPv6 message codec"`

### Task 4: Frames and router advertisements

**Files:**
- Create: `lib/dhcp6_client/include/dhcp6_frame.h`, `lib/dhcp6_client/src/dhcp6_frame.cpp`
- Test: `test/native/test_dhcp6_frame/test_main.cpp`; add to `scripts/sanitize.sh`

**Interfaces:**
- Produces:
  - `size_t dhcp6Frame(const uint8_t mac[6], const uint8_t linkLocal[16], const uint8_t* payload, size_t len, uint8_t* out, size_t size);` Ethernet to `33:33:00:01:00:02`, IPv6 src = link-local, dst `ff02::1:2`, hop limit 1, UDP 546 → 547 with checksum. 0 if it does not fit.
  - `bool dhcp6FromFrame(const uint8_t* frame, size_t len, const uint8_t mac[6], const uint8_t** payload, size_t* payloadLen);` true only for Ethernet dst = `mac`, EtherType 0x86DD, next header UDP (no extension headers), UDP 547 → 546, lengths consistent, UDP checksum valid.
  - `struct RouterAdvert { bool managed; bool other; };` `bool raRead(const uint8_t* frame, size_t len, RouterAdvert& out);` true only for ICMPv6 type 134 code 0, hop limit 255, source in fe80::/10, valid ICMPv6 checksum.
- [ ] **Step 1: Failing tests:** `test_frame_round_trips_through_from_frame`, `test_frame_checksum_matches_device_capture` (build `DEVICE_INFO_REQUEST`'s payload with its link-local and compare checksum and headers byte for byte), `test_from_frame_reads_kea_reply`, `test_from_frame_rejects_other_mac`, `test_from_frame_rejects_bad_checksum`, `test_from_frame_rejects_extension_headers`, `test_ra_reads_lan_router_without_m` (`ROUTER_RA_LAN`: managed false), `test_ra_reads_testnet_m_flag` (`TESTNET_RA_M`: managed true), `test_ra_rejects_hop_limit_below_255`, `test_ra_rejects_global_source`, truncation and single-byte-corruption tests for both readers.
- [ ] **Step 2:** Run; expected FAIL. **Step 3:** Implement (reuse the checksum approach of `lib/dhcp_option`). **Step 4:** Native + sanitizers PASS.
- [ ] **Step 5: Commit** `git commit -m "dhcp6_client: frames and router advertisement flags"`

### Task 5: Client state machine

**Files:**
- Create: `lib/dhcp6_client/include/dhcp6_client.h`, `lib/dhcp6_client/src/dhcp6_client.cpp`
- Test: `test/native/test_dhcp6_client/test_main.cpp`; add to `scripts/sanitize.sh`

**Interfaces:**
- Consumes: `Dhcp6Out`, `Dhcp6In`, `Dhcp6Lease`, `dhcp6Build` (Task 3).
- Produces:
  - `enum class Dhcp6State : uint8_t { Idle, Soliciting, Requesting, Bound, Renewing, Rebinding, Declining };` `const char* dhcp6StateName(Dhcp6State);`
  - `struct Dhcp6Action { enum Kind : uint8_t { None, Send, AddAddress, RemoveAddress } kind; size_t len; uint8_t addr[16]; uint32_t preferred; uint32_t valid; };` (`len`: bytes written to the poll buffer for `Send`)
  - `class Dhcp6Client` with `Dhcp6Client(const uint8_t mac[6], uint32_t (*random32)())`, `void setHostname(const char* name)`, `void start(uint32_t nowMs)` (no-op if running; keeps running until reboot once started), `void received(const Dhcp6In& m, uint32_t nowMs)` (ignores wrong xid, wrong server id after selection), `void addressDuplicate(uint32_t nowMs)`, `Dhcp6Action poll(uint32_t nowMs, uint8_t* buf, size_t size)` (call until `None`), `Dhcp6State state() const`, `bool hasLease() const`, `const Dhcp6Lease& lease() const`, `uint32_t validLeftS(uint32_t nowMs) const`, counters `uint32_t sent() const`, `uint32_t accepted() const`, `uint32_t ignored() const`.
  - `int dhcp6PickSlot(const uint8_t* states, int count);` first slot whose lwIP state is invalid (0) except slot 0 (link-local); -1 if none.
- Behaviour pinned by tests (RFC 8415): first Solicit after a random 0-1 s delay; Advertises collected for the first SOL_TIMEOUT, highest preference wins, preference 255 ends collection at once; Request retransmitted up to REQ_MAX_RC then back to Solicit; Reply with lease → `AddAddress`, state Bound; T1/T2 zero → 0.5 and 0.8 of preferred; at T1 Renew (with server id), at T2 Rebind (without), at valid end `RemoveAddress` and Solicit; `addressDuplicate` → `RemoveAddress`, Decline up to DEC_MAX_RC, then Solicit; `NoAddrsAvail`/`NotOnLink`/`NoBinding` handled as in RFC 8415 18.2.10; all timers wrap-safe.
- [ ] **Step 1: Failing tests:** `test_client_sends_nothing_before_start`, `test_first_solicit_within_max_delay`, `test_highest_preference_advertise_wins`, `test_preference_255_requests_immediately`, `test_reply_adds_address_with_lifetimes`, `test_client_ignores_reply_with_wrong_xid`, `test_renew_at_t1_rebind_at_t2`, `test_zero_t1_t2_use_half_and_eighty_percent`, `test_address_removed_at_valid_end`, `test_duplicate_address_is_declined_and_solicit_restarts`, `test_client_returns_to_solicit_on_noaddrsavail`, `test_request_gives_up_after_max_rc`, `test_solicit_backs_off_to_max_rt_and_survives_millis_wrap` (simulate 10 h with a fake clock starting at `0xFFFF0000`; intervals never exceed 3600 s × 1.1), `test_pick_slot_skips_link_local_and_used_slots`, `test_pick_slot_returns_none_when_all_used`, `test_every_state_has_a_name`. Use a deterministic `random32` in tests.
- [ ] **Step 2:** Run; FAIL. **Step 3:** Implement. **Step 4:** Native + sanitizers PASS.
- [ ] **Step 5: Commit** `git commit -m "dhcp6_client: RFC 8415 client state machine for one IA_NA address"`

### Task 6: Firmware integration

**Files:**
- Create: `src/dhcp6_service.h`, `src/dhcp6_service.cpp`
- Modify: `src/net_stats.cpp` / `src/net_stats.h` (call `dhcp6InspectFrame` in `countingInput` before lwIP; add `bool netStatsSendFrame(const uint8_t* frame, size_t len)` that allocates a `PBUF_RAW` pbuf and calls `originalLinkOutput`, counted like other output), `src/main.cpp` (`dhcp6Begin(eth.getNetIf())` after `ipv6StaticBegin`, `dhcp6Poll()` in `loop()` next to `netStatsPoll`, `(dhcpv6)` label in the IPv6 listing next to `(static)`), `platformio.ini` only if a flag is needed (none expected).

**Interfaces:**
- Consumes: Tasks 3-5; `hostnameCurrent()` (`src/hostname.h`); `get_rand_32()` (pico_rand) as `random32`; the lwIP lock used by `src/ipv6_static.cpp`.
- Produces: `void dhcp6Begin(netif* nif)`, `void dhcp6Poll()`, `void dhcp6InspectFrame(const uint8_t* frame, size_t len)` (lwIP context: parse RA flags and DHCPv6 messages into a one-entry static mailbox; nothing else), `bool dhcp6IsSlot(int i)`, `struct Dhcp6Status { Dhcp6State state; bool hasAddress; char address[IPV6_TEXT_MAX]; uint32_t validLeftS; const char* problem; uint32_t sent, accepted, ignored; };` `void dhcp6GetStatus(Dhcp6Status& out)`.
- Notes: frame and message buffers static (no stack buffers over 256 bytes); `AddAddress` via `netif_add_ip6_address` into `dhcp6PickSlot`'s slot with `netif_ip6_addr_set_valid_life`/`_pref_life`; DAD outcome read each poll (duplicate → `addressDuplicate`); `problem` = "no free IPv6 address slot" when the pick fails; M flag only from RAs `raRead` accepts.
- [ ] **Step 1:** Implement; build `pio run -e pico` with no warnings; native suites still PASS.
- [ ] **Step 2:** Flash; run the existing HIL suites (`test_ipv6.py`, `test_snmp.py`, `test_web.py`, `test_serial_menu.py`, `test_watchdog.py`, community swap as in earlier work) with no M-flag network: all PASS unchanged (Review Focus: nothing changes without M).
- [ ] **Step 3: Commit** `git commit -m "DHCPv6 client in the firmware (frames via the netif hooks)"`

### Task 7: Status screen and web page

**Files:**
- Modify: `src/cli.cpp` (`printStatus`: `DHCPv6: <state>, <address> (valid <n> s), sent/accepted/ignored`, or `DHCPv6: off (no router asked for it)`, plus `problem` when set), `test/hil/device.py` (parse `dhcp6State`, `dhcp6Address`), `lib/web_ui/include/web_app.h` (`WebStatus`: `char ipv6Dhcp[46]; uint32_t ipv6DhcpValidS;`), `lib/web_ui/src/web_app.cpp` (status page row "IPv6 (DHCPv6)" when set), `src/web_service.cpp` (fill the fields)
- Test: `test/native/test_web_ui/test_main.cpp`: `test_status_page_shows_dhcpv6_address_when_present`, `test_status_page_has_no_dhcpv6_row_without_address`
- [ ] Steps: failing web tests → implement → native PASS → build → commit `git commit -m "DHCPv6 address on the status screen and web page"`

### Task 8: End-to-end on the test network, docs

**Files:**
- Create: `test/hil/test_dhcp6.py` (opt-in `RFUPS_DHCP6_TEST=1`, uses `TestNet` from Task 1)
- Modify: `docs/TESTING.md` (test network setup, the suite), `README.md` (IPv6 section: DHCPv6 bullet; status example), `docs/LIBRENMS.md` only if relevant, `CHANGELOG.md` ([Unreleased] Added), `TODO.md` (tick the DHCPv6 item with test names and date), `CLAUDE.md` (Layout: `lib/dhcp6_client`, `src/dhcp6_service.cpp`)
- [ ] **Step 1: Tests:** `test_managed_flag_gets_an_address_from_kea` (address in Kea's pool appears as `(dhcpv6)`, passes DAD, ping6 and SNMP get answer on it), `test_lease_is_renewed` (valid 120 s / preferred 60 s: a Renew in Kea's log and the address still there after 90 s), `test_address_removed_when_server_stops` (stop Kea, keep the RA; address gone within valid + 10 s), `test_duplicate_address_is_declined` (Kea reserves the PC's own address for the device: Decline in Kea's log, no such address on the device), `test_stack_headroom_after_dhcpv6` (status screen core 0 stack never-used > 1 KB after the exchanges; the project's on-device stack measurement for this path), `test_lan_untouched` (another LAN host's view: the PC's own IPv6 routes unchanged before/after).
- [ ] **Step 2:** Run with `RFUPS_DHCP6_TEST=1`; PASS. Run all native, embedded (`pio test -e pico`, reflash after) and sanitizers.
- [ ] **Step 3:** Docs as listed; commit `git commit -m "DHCPv6 end to end on the test network, docs"`
