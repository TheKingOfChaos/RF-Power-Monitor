# Testing

The project has three test layers. Each one tests what the layer below cannot.

| Layer | Folder | Runs on | Framework | Command | Time |
|---|---|---|---|---|---|
| Unit | `test/native/` | Your PC | Unity | `pio test -e native` | ~5 s |
| On-device | `test/embedded/` | The Pico | Unity | `pio test -e pico` | ~55 s |
| Hardware-in-the-loop (HIL) | `test/hil/` | PC talking to flashed Pico | Python `unittest` | see below | several minutes (longer with mains tests) |

The rules for writing tests are in [.clinerules](../.clinerules), section *Testing Requirements → Test Rules*.

![Test layers: from native unit tests to LibreNMS with real mains, and what CI runs](diagrams/test-layers.svg)

## CI

[.github/workflows/tests.yml](../.github/workflows/tests.yml) runs on every push and pull request on GitHub: the native unit tests, the same tests under AddressSanitizer/UBSan, the SNMP agent against net-snmp (`scripts/host_agent_check.sh`, including MIB validation), the tool tests (`test/tools`), and a firmware build that uploads `firmware_full.uf2` (boot program and slot A, the file for the BOOTSEL drive) as an artifact. It also compiles the on-device test programs. The on-device and HIL tests need the Pico and are run locally.

## One-time setup

### Host compiler (for `native`)

The native tests need `g++` on `PATH`. On Windows, install MinGW-w64 (WinLibs):

```powershell
winget install --id BrechtSanders.WinLibs.POSIX.UCRT -e
```

Restart VS Code (or open a new terminal) afterwards so `PATH` is refreshed, then check with `g++ --version`.

### USB driver (for flashing)

`pio run -t upload` and `pio test -e pico` use picotool, which needs the WinUSB driver on the Pico's boot interface. Do this once per PC:

1. Hold BOOTSEL while plugging in the Pico (the `RPI-RP2` drive appears).
2. Run [Zadig](https://zadig.akeo.ie), **Options → List All Devices**.
3. Select **RP2 Boot (Interface 1)** (USB ID `2E8A 0003 01`). Do not pick Interface 0, that is the drive.
4. Target driver **WinUSB**, click **Install Driver**, then replug.

### Serial port

The Pico is expected on `COM16` (set in `platformio.ini`; the HIL tests read `RFPOWER_PORT`). Close PuTTY or any serial monitor first: an open port gives "Access denied" and the upload fails.

## Running the tests

`pio` is not on `PATH` by default. Use the full path, or the PlatformIO terminal / Testing panel in VS Code.

`platformio.ini` has `default_envs = pico`, so a bare `pio run`, `pio test` or upload builds `pico` only. The `pico_b` environment (the same firmware linked for slot B) is built by `tools/fw_release.py` with `-e pico_b`, and an upload to it is refused. `pio test -e native` and the CI jobs name their environment and are unaffected.

**RAM limit.** The 50 % rule (`.clinerules`) is checked against PlatformIO's `RAM:` line of `pio run -e pico` (`.data` + `.bss`). Anything a code path allocates from the heap counts on top of it at that path's peak, so large buffers are static; the firmware update allocates nothing (its receiver is a static 632 bytes). The status screen's `RAM used` is a different figure: it also counts both stacks (core 1's 8 KB stack is allocated by the core) and the heap in use.

**Flashing a Pico by hand.** The Pico's flash holds the boot program, slot A and slot B (see the user manual). `pio run -e pico -t upload` and `pio test -e pico` send `firmware_full.uf2` (boot program, boot record and slot A). Copy only that file, or a release's `-usb.uf2`, to a BOOTSEL drive: the plain `firmware.uf2` the platform also writes has no boot program, and a device flashed with it does not start. `pio test -e pico` replaces the firmware with a test program; afterwards `pio run -e pico -t upload` puts the normal firmware back (slot A behind the boot program, kept).

```powershell
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"

# Unit tests on the PC, no hardware needed
& $pio test -e native

# On-device tests: flashes each test program to the Pico in turn
& $pio test -e pico

# One suite only
& $pio test -e native -f native/test_netconfig
& $pio test -e pico   -f embedded/test_settings_store
```

### Tool tests (`test/tools`)

Pure Python, no hardware: the firmware signing tools (`tools/fw_keygen.py`, `fwsign`, the `.upd` layout), `tools/fw_update.py` against a fake device, `scripts/uf2.py` and the linker script generation and flash layout checks in `scripts/slot_build.py` (slot B before LittleFS, the Python constants equal `fw_layout.h`). They need a host `g++`/`gcc` (fwsign is built into `.pio/fwsign` on first use) and, for the linker script tests, the Arduino-Pico core (skipped when absent). Key generation is only tested with `--out` into a temp dir, never with the real field key.

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" -m unittest discover -s test/tools -v
```

### Sanitizers (memory safety)

Code that parses data from the network must be free of out-of-bounds reads and undefined behaviour. MinGW has no sanitizers, so the native suites are also built with gcc in WSL using AddressSanitizer and UndefinedBehaviorSanitizer:

```powershell
& $pio test -e native     # once, downloads Unity and ArduinoJson
wsl -d Ubuntu-24.04 -- bash scripts/sanitize.sh
```

Every native suite must be listed in `scripts/sanitize.sh`; the script fails if one is missing.

### SNMP agent against net-snmp, without hardware

`scripts/host_agent_check.sh` builds the SNMP agent libraries into a small UDP server (`test/host/host_agent.cpp`, with ASan/UBSan), runs it on 127.0.0.1 in WSL and checks the output of the real `snmpget`, `snmpgetnext`, `snmpwalk`, `snmpbulkget`, `snmpbulkwalk` and `snmpset`, in v1 and v2c, and that v3 gets no reply. Use it after changing `lib/snmp_ber` or `lib/snmp_agent`, before flashing:

```powershell
wsl -d Ubuntu-24.04 -- bash scripts/host_agent_check.sh
```

It needs the `snmp` package in WSL. If `snmp-mibs-downloader` is installed too (it is on this PC), it also validates `mibs/RF-POWER-MONITOR-MIB.txt`: `snmptranslate -PW` must report no warnings in our file, every object must be in the tree, and `snmpget` must show MIB names (`mainsPowerStatus.0 = INTEGER: present(1)`). Without that package the MIB checks are skipped.

### HIL tests

These run against the real firmware, so flash it first:

```powershell
& $pio run -e pico -t upload
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" -m unittest discover -s test/hil -v
```

They use the Python and `pyserial` that come with PlatformIO, so there is nothing to install. `RFPOWER_WSL_DISTRO` names the WSL distribution with net-snmp (default `Ubuntu-24.04`). The SNMP tests (`test_snmp.py`) also call net-snmp in WSL (`wsl -d Ubuntu-24.04 -- snmpget ...`) and send raw UDP from Windows; they read the device IP from the serial status screen. Run one file with `-p`:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" -m unittest discover -s test/hil -p "test_snmp.py" -v
```

The device must be in DHCP mode when the run starts, and is left in DHCP mode. The menu is available before the network is up, so tests that need the network call `Device.wait_connected()` first. The static IP test is skipped unless you give it a free address:

```powershell
$env:RFPOWER_STATIC_IP = "10.10.6.250"   # must not be in use on the LAN
$env:RFPOWER_STATIC_MASK = "255.255.0.0" # optional, default 255.255.0.0
$env:RFPOWER_STATIC_GW = "10.10.0.1"     # optional, default: current DHCP gateway
$env:RFPOWER_MANUAL_MAINS = "1"          # optional, mains off/on tests where you switch mains when asked
$env:RFPOWER_MANUAL_BUTTON = "1"         # optional, interactive factory reset button test
$env:RFPOWER_DHCP_TRAP_TARGET = "10.10.6.65" # optional: the address your DHCP server sends in option 224
$env:RFPOWER_STATIC_IPV6 = "fd16:d833:4f0e:3ece::249" # optional: free address in the device's /64 (static IPv6 test)
$env:RFPOWER_STATIC_IPV6_GATEWAY = "fe80::1"         # optional: gateway for that test (stored and shown only)
```

**Failures caused by the test PC, not the firmware.** Check these before you look for a firmware bug:

- **A VPN client (for example Cisco AnyConnect) is connected.** It can take over the routes to the LAN's IPv6 prefix, even link-local, and then `test_ipv6.py` fails: ping6 says "General failure", SNMP and the web page get no answer over IPv6, and `test_zz_device_did_not_restart_during_the_tests` fails as well. Check with `ping -6 <device IPv6 address>` and disconnect the VPN for the run.
- **Windows' Host Network Service (WSL, Docker Desktop) is running.** It runs its own DHCP server on UDP port 67 (svchost, services SharedAccess and HNS), and Windows then does not let other programs' packets from source port 67 reach the LAN; WSL in mirrored mode cannot bind the port either. `test_dhcp_trap.py`'s `test_forged_dhcp_ack_with_option_224_is_ignored` sends its forged ACK from port 67, so it fails on such a PC (2026-11-02: 0 of 20 datagrams from port 67 reached the device, all from a random port did). The firmware's check of forged ACKs is covered by `test/native/test_dhcp_option`; run this HIL test from a PC without WSL/Docker networking, or with those services stopped.

### Mains tests with a smart plug

The mains tests switch the device's mains input off and on. They run automatically when the mains detection circuit is fed from an outlet on an IKEA Dirigera hub ([test/hil/mains_switch.py](../test/hil/mains_switch.py)):

```powershell
$env:RFPOWER_MAINS_SWITCH = "dirigera"
$env:RFPOWER_DIRIGERA_IP = "10.10.6.124"        # the hub
$env:RFPOWER_DIRIGERA_OUTLET = "Stikprop 1"     # outlet name in the IKEA app
# optional, paths on the WSL side, these are the defaults:
$env:RFPOWER_DIRIGERA_TOKEN = "~/dirigera/token"
$env:RFPOWER_DIRIGERA_PYTHON = "~/dirigera/.venv/bin/python"
```

One-time setup in WSL: `python3 -m venv ~/dirigera/.venv && ~/dirigera/.venv/bin/pip install dirigera`, then `~/dirigera/.venv/bin/generate-token <hub ip>` (press the button on the hub) and save the token in `~/dirigera/token` with `chmod 600`. Never commit the token. The tests call [test/hil/dirigera_outlet.py](../test/hil/dirigera_outlet.py) with that Python and always switch the outlet back on, also when a test fails. One switch takes about a second; the device sees the change about 0.1 s after the hub confirms it.

Without `RFPOWER_MAINS_SWITCH`, `RFPOWER_MANUAL_MAINS=1` runs the same tests with prompts for you to switch mains by hand (except the short-outage test). With neither, they are skipped.

The trap tests (`test_snmp_traps.py`) set the device's trap destination to this PC and listen on UDP 162. Windows Firewall asks once whether PlatformIO's Python may receive network traffic; allow it on the network the Pico is on, or no traps arrive. The tests restore the previous trap destination.

Flashing an embedded test replaces the firmware, so always reflash with `pio run -e pico -t upload` before HIL tests or normal use.

### DHCPv6 test network (`RFPOWER_DHCP6_TEST=1`)

The DHCPv6 tests (`test_dhcp6_testnet.py`, `test_dhcp6.py`) need a router that sets the M flag and a DHCPv6 server, which an ordinary LAN does not have. [test/hil/dhcp6_testnet.py](../test/hil/dhcp6_testnet.py) makes one on this PC without disturbing the LAN, in WSL (mirrored networking) as root:

- [send_ra.py](../test/hil/dhcp6/send_ra.py) sends router advertisements **unicast to the device's link-local address only**, with the M (or O) flag, router lifetime 0 and no prefix, so no other host sees them and the device keeps its routes and SLAAC address.
- Kea ([kea-dhcp6.conf.in](../test/hil/dhcp6/kea-dhcp6.conf.in)) drops every packet not sent from the device's link-local address and gives addresses only to the device's DUID, from a small pool (`<prefix>::d600:1-ff`) with short lifetimes.
- [testnet.sh](../test/hil/dhcp6/testnet.sh) starts and stops both and, while they run, adds the `fe80::/64` route that mirrored networking leaves out (without it Kea cannot reply). The tests always stop them.

One-time setup: `wsl -d Ubuntu -u root -- apt install python3-scapy kea-dhcp6-server`, then `systemctl disable --now kea-dhcp6-server` (the package starts a service; the tests run their own Kea). The tests run commands as root with `wsl -u root`. A VPN such as Cisco AnyConnect blocks IPv6 between this PC and the device ("General failure" on ping6): disconnect it for these tests and for `test_ipv6.py`.

### LLDP switch (`RFPOWER_LLDP_SWITCH`, `RFPOWER_LLDP_SWITCH_CONSOLE`)

The LLDP tests (`test_lldp.py`) need the device plugged into a switch that sends LLDP; other networks may have none, so they are opt-in:

```powershell
$env:RFPOWER_LLDP_SWITCH = "1"                # the device's switch sends LLDP: the status screen must name it
$env:RFPOWER_LLDP_SWITCH_CONSOLE = "COM45"    # optional: the switch's serial console (Junos, 9600 baud)
$env:RFPOWER_LLDP_SWITCH_CREDENTIALS = "..."  # optional, default secret/switch.md
```

With the console, the LLDP tests log in, run `show lldp neighbors` (operational commands only, the configuration is never touched) and log out. The login file has the lines `username: ...` and `password: ...`; keep it in `secret/` (git-ignored) and never commit it. The tests never print it. A root login lands in the shell; the helper starts `cli` itself.

The hidden menu command `lldpdump` prints the last LLDP frame from the switch in hex (`LLDP 0180c2...`), for test vectors; `test/native/test_lldp` has one from a Juniper EX2200-C (its serial number replaced).

### Switch port changes (`RFPOWER_SWITCH_CONFIG`)

`test_switch_port.py` makes the "no link" and "no DHCP server" states by changing the device's switch port over the same console, so it needs `RFPOWER_LLDP_SWITCH_CONSOLE` and, because it changes the switch's configuration, its own opt-in:

```powershell
$env:RFPOWER_SWITCH_CONFIG = "1"              # allow the tests to change the device's switch port
$env:RFPOWER_LLDP_SWITCH_CONSOLE = "COM45"
```

The port is the one the device names over LLDP, used only if the switch also lists the device's MAC on it. Only that port is changed: `disable`, or an input filter `rfpower-test-no-dhcp` that drops UDP to port 67 in place of its own filter (whose policer is off for those minutes). Changes are made under `configure exclusive`, refused if the candidate already holds someone else's uncommitted changes, and committed with `commit confirmed 5`: if a test dies, Junos rolls the change back by itself within 5 minutes. The test restores the port with a plain commit. The device stays powered over USB, so the serial menu works while the port is down. The two tests take about 4 minutes.

### Firmware update tests (`RFPOWER_FW_UPDATE_TEST=1`)

`test/hil/test_fw_update.py` has `BootProgramTest` (always runs, reboots once) and `FirmwareUpdateTest`, which uploads signed `.upd` files over the web page, lets trials roll back and crashes the firmware on purpose. It is slow and replaces the running slot many times, so it is opt-in:

```powershell
$env:RFPOWER_FW_UPDATE_TEST = "1"
$env:RFPOWER_COMMUNITY = "rfit"            # the device's SNMP community, default public
$env:RFPOWER_WEB_PASSWORD = "..."          # only when the device has its own web password; else the default one on the status screen is used
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" -m unittest discover -s test/hil -p "test_fw_update.py" -v
```

- The device must be in DHCP mode and trust the development key (`keys/dev`, the usual build). The tests never change its settings; every test ends on a development-signed build of the current version, kept, in either slot (the boot program test follows the running slot).
- `setUpClass` builds the images once into `.pio/fwtest/` (`crash.upd`, `shorttrial.upd` with `tools/fw_release.py --test-variant`, `normal.upd`, and `otherkey.upd` signed with a throw-away key) and reuses them while they are newer than `src/`, `lib/`, `include/`, `boot/`, `scripts/` and `platformio.ini` (`RFPOWER_FW_REBUILD=1` forces it). A cold build takes about 10 minutes, a warm run about 11 minutes: upload to kept 25 s, crash rollback 38 s, unreachable rollback 80 s, crash-loop fallback 2 minutes.
- The serial port observes and never counts as a request. A poller with the community (LibreNMS) or a web login confirms a trial within seconds, so the tests accept "kept" where a trial may already be confirmed. The `shorttrial` variant never confirms and ends its trial after 60 s of network time, which is how the refusal during a trial and the unreachable rollback are tested (the `noconfirm` variant is not needed: `shorttrial` does not confirm either).
- Traps: the tests use the receiver of `test_snmp_traps.py` and need the PC to be the device's trap destination. When UDP 162 cannot be bound (LibreNMS's snmptrapd holds it) only the trap assertions are skipped and the run prints a note. Do not stop LibreNMS for it: read what arrived from LibreNMS instead, read-only, e.g. `docker exec lnms-test-db-1 sh -c 'mysql -ulibrenms -p"$MYSQL_PASSWORD" librenms -e "select datetime,type,left(message,120) from eventlog where datetime > \"<start>\" order by event_id"'` (the password comes from the container's own environment). LibreNMS logs only the mains traps as type `trap`; a firmware trap triggers a poll, so look for `State sensor Firmware ...` entries, and `docker logs lnms-test-snmptrapd-1` shows each trap that reached snmptrapd as one handler line.
- The tests read the community from `RFPOWER_COMMUNITY`; the other HIL files still expect `public` and skip on a device that uses another community.
- After each rollback (crash and unreachable) the tests check that the other slot reads `empty`: the firmware erases a release that was rolled back. Refusals (409 during a trial, 401 with an expired session) are sent with a full-size body, so they also check that the answer reaches the client instead of a reset connection.
- `test_power_cycles_during_trial_do_not_roll_back` also needs `RFPOWER_POE_CYCLE=1` and `RFPOWER_LLDP_SWITCH_CONSOLE`: it changes PoE on the device's own switch port (`set poe interface <port> disable`, commit confirmed 5, then `delete poe interface <port>`, four times, about 3 minutes). The port must have no PoE statement of its own (the switch has `interface all;`). With USB attached the Pico stays powered, so it covers PoE and link loss.

### Update site tests (`update-server/tests`, `RFPOWER_UPDATE_SERVER_TEST=1`)

The site is standard-library Python, so its tests run anywhere (also in CI), without Docker or hardware:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" -m unittest discover -s update-server/tests -v
```

They cover the wire format against `update-server/tests/vectors.json` (the same vectors as the firmware's `test/native/test_update_protocol`), the store and the assignment rules (done, rolled back, refused, three download failures, lost after 2 hours, replayed nonces, offline after 65 minutes), and the web pages against a real server on a free port (login, rate limit, CSRF, escaping of device text, upload limits, release downloads).

`test/hil/test_update_server.py` runs the site on this PC (port 8072, a temporary data directory and key), points the device at it and goes end to end: check-in, wrong key (`badauth`), an unreachable site while SNMP keeps answering within 100 ms, a release with broken signatures (refused, the device keeps running), and a real update to a `3.4.1-hil` build and back to the current version through the site, each kept after a check-in confirms its trial. It is opt-in because it updates the device twice (about 75 s with warm builds):

```powershell
$env:RFPOWER_UPDATE_SERVER_TEST = "1"
$env:RFPOWER_SNMP_COMMUNITY = "rfit"   # the device's community, default public
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" -m unittest discover -s test/hil -p "test_update_server.py" -v
```

- The device must run the current version (development key), kept; it ends on that version, kept, with no update server set. A refused test release leaves the other slot `empty`, as any refused upload does.
- The test builds `.pio/fwtest/3.4.1-hil.upd` and `.pio/fwtest/<current>.upd` once with `tools/fw_release.py --test-version` (delete them after source changes). These builds use other build flags: run `pio run -e pico -t upload` afterwards before flashing anything else.
- The device must reach this PC on TCP 8072 (Windows firewall; Python was allowed here). `RFPOWER_UPDATE_SERVER_HOST` overrides the address the device is given, `RFPOWER_UPDATE_SERVER_PORT` the port. Nothing else may listen on that port.
- The hidden menu commands `checkin` (check in now) and `fetchtest <url>` (one GET with the update client, result printed) exist for these tests and for debugging.

## What is covered

### `test/native/test_update_protocol`, `test/native/test_update_client`: the update site client (`lib/update_client`)

- The check-in body and the reply's keyed BLAKE2b MAC, byte for byte against Python's `hashlib` (`update-server/tests/vectors.json`); a reply is only read after its MAC and the check-in's nonce verify; every truncation, a changed byte, another nonce, CR/NUL, over 512 bytes and unknown status lines are refused.
- The URL (`http://` only, host, `[IPv6]`, port 1-65535, path, 127 characters), the HTTP response head (status, one Content-Length, no chunked encoding, 1024-byte head limit, every truncation), the `.upd` header version (golden file) and version order.
- Check-in timing (60 s after the network, 30 min, 5 min retry, 60 s report, millis() wrap-around), the update settings JSON (round trip, worst case, truncations) and how menu and web changes are applied (bad URL or key changes nothing; `clear` wipes the key).
- A download: the running version is not downloaded again; the file's header and its **signed** slot header must name the version the site asked for; failures are reported once (`<v> download <why>`, `<v> refused <why>`).

### `test/native/test_dhcp_option`: option 225

The update server URL is read as text from an accepted DHCPACK (1-127 printable bytes, never truncated); the captured device requests have room to ask for both 224 and 225.

### `test/native/test_netconfig`: `lib/netconfig`
- IPv4 parsing: valid addresses, octet range, octet count, stray characters, signs, whitespace, empty and null input, output left untouched on failure
- Subnet masks: every prefix /1 to /32 accepted, zero and non-contiguous masks rejected
- Static configuration: first failing field reported, network/broadcast host rejected, /31 and /32 handled, gateway outside the subnet or equal to the host rejected
- IPv6 parsing: full and compressed forms, `::` at start/middle/end and for one group, upper case, the 39-character maximum; malformed text (`:::`, two `::`, 7 or 9 groups, 5-digit groups, `/64`, `%zone`, IPv4 tails, spaces, 40 characters) refused with the output untouched; every truncation of an address
- IPv6 formatting (RFC 5952): longest zero run as `::`, first run on a tie, single zero group kept, lower case
- Static IPv6: empty = none, gateway without address refused; ::/8, link-local, multicast and zero interface id refused; gateway must be unicast, link-local or in the /64, and differ from the address

### Hostnames in `test/native/test_netconfig`
- Hostname rules (length 1-32, letters/digits/'-', no leading or trailing '-', one label, ASCII only)
- Free text to hostname: valid names unchanged, runs of other characters become one '-', cut at 32 on a word end, fallback name; every printable text gives a valid hostname

### `test/native/test_dhcp_option`: DHCP option 224 (`lib/dhcp_option`)
- Real frames: the device's own DISCOVER and REQUESTs (dumped at the driver), the LAN server's OFFER/ACK, and Kea 2.2.0's replies to the device's frames replayed in Docker with and without option 224 added (`frames.h`). Kea sent 224 only to the edited frames, which proves the request works
- Editing: 224 appended to option 55 in every device request, length unchanged, nothing else changed, UDP checksum recomputed (not dropped), idempotent, zero checksum kept, no padding = no change, server messages / ARP / corrupt frames untouched, every truncation and every single-byte corruption safe
- (see also `test_access_control`: IPv4 key = address, IPv6 key = full address, IPv4-mapped = IPv4, ::/32 refused, hosts in one /64 have their own buckets and a manager's trust does not pass to its /64 neighbours, RFC 5952 log formatting)
- Reading: Kea ACK gives 10.10.6.65, the lease, the transaction id of the device's request and the server id, ACKs without 224, OFFERs and requests are not ACKs, another MAC, wrong option length, bad IP/UDP checksums, fragments, every truncation and corruption

### `test/native/test_settings_codec`: `lib/settings_codec`
- Defaults and JSON round trip, maximum-length fields fit the buffer
- v1 files without `sequence` still load (backward compatibility)
- Rejection of empty, malformed, wrong-version, mistyped, over-long and unprintable data, with no truncation
- Static configuration is validated only when DHCP is off
- Sequence comparison across 32-bit wrap-around, A/B slot selection, and fallback when the newer copy is invalid
- sysName/sysContact/sysLocation: defaults, round trip with characters JSON escapes, worst-case size fits, older files load with defaults, over-long/non-printable/mistyped values rejected, empty allowed; `settingsTextIsValid` boundaries
- Static IPv6: empty by default and for files without the fields, round trip and equality, works with DHCP for IPv4, invalid/over-long/mistyped values rejected

### `test/native/test_snmp_ber`: `lib/snmp_ber`
- OID text parsing and formatting, X.690 rules for the first two arcs, 32-bit arcs, arc limit
- Lexicographic OID ordering (numeric, prefix first) and prefix matching, for GetNext
- Minimal encodings of INTEGER (sign boundaries, INT32_MIN/MAX), Counter32/Gauge32/TimeTicks (leading 0x00), OCTET STRING, NULL, OID, IpAddress
- Short and long form lengths, nested sequences growing past 127 bytes, buffer-full and nesting errors
- Byte-exact encode and decode of requests captured from net-snmp (`snmpget`, two varbinds)
- Strict decoding: truncation at every length, lengths beyond the buffer, indefinite/oversized lengths, multi-byte tags, wrong tags, bad INTEGER/unsigned/NULL/OID encodings, child readers confined to their region, sticky errors
- Every single-byte corruption of a real request (~15 000 packets) decodes safely; run `scripts/sanitize.sh` to check this under AddressSanitizer
- Counter64: minimal encoding from 0 to 2^64-1 (leading 0x00 when the top bit is set), round trip, oversized values rejected

### `test/native/test_snmp_message`: `lib/snmp_agent` message layer
- net-snmp Get, GetNext and two-varbind requests parsed exactly; SetRequest accepted for the agent to reject
- version 2 (unassigned) and v3 → BadVersion, response/trap/unknown PDUs → UnsupportedPdu, trailing bytes, every truncation, varbind without value, garbage → Malformed
- Varbind limit (16), empty varbind list
- GetResponse byte-exact; every value type; unknown value type and full buffer fail cleanly
- Error responses echo the request varbinds byte for byte; community preserved byte for byte
- Every single-byte corruption of a real request parses safely

### `test/native/test_snmp_agent`: MIB and request handling
- MIB strictly sorted, expected objects, exact match and next-object lookup (non-leaf and between objects)
- GetRequest of every object with type and value from the device state; mains lost; empty strings
- noSuchName with the right error-index for unknown and non-leaf OIDs
- Full walk from `1.0` (LLDP-MIB first, with a neighbour) visits every object in order and ends with noSuchName; enterprise subtree; several GetNext varbinds
- SetRequest → noSuchName with the value echoed
- No response for wrong community (several near-misses), changed community, v3 and unknown versions, response PDUs, malformed packets
- tooBig when the response does not fit, nothing when even the error does not fit, 16 varbinds fit
- Statistics, request id echo across the int32 range, every single-byte corruption yields either nothing or a valid response

### v2c in the native suites
- `test_snmp_message`: net-snmp v2c Get and GetBulk parsed (non-repeaters/max-repetitions), GetBulk refused in v1, Inform/v2-Trap/Report refused, v2c response byte-exact with an exception value, `tryAdd` rollback, every single-byte corruption of a GetBulk request
- `test_snmp_agent`: v2c Get with noSuchObject/noSuchInstance and no error-status, GetNext endOfMibView, full v2c walk, Set notWritable/noCreation, v1 unchanged for the same requests; GetBulk against the net-snmp request, interleaved repeaters, whole MIB in one request, zero/negative/oversized parameters, INT32_MAX repetitions bounded, truncation, tooBig only when nothing fits, v2c tooBig with empty list, corruption
- `test_snmp_trap`: v2c coldStart, mainsPowerLost and mainsPowerLostTooLong byte-exact against `snmptrap -v2c`, trap OIDs of authenticationFailure, mainsPowerLost and mainsPowerRestored
- `test_settings_codec`: trap version default v2c, round trip, invalid values rejected

### IF-MIB and temperatures in the native suites
- `test_if_stats` (`lib/if_stats`): frame classification (unicast, multicast, broadcast, runt frames), receive verdicts (IPv4/ARP delivered, IPv6 only when enabled, other EtherTypes unknown, length boundaries 13/14/1514/1515), each received frame in exactly one counter, discards, octet and packet counting, 64-bit counters past 2^32, W5500 PHYCFGR decoding (link, 10/100, duplex) and speed in bit/s
- `test_snmp_agent`: ifTable and ifXTable values from the device state, ifInDiscards/ifInErrors/ifInUnknownProtos, ifPhysAddress is the MAC, Counter64 values, link down shows `down` and speed 0, no such instance for interface 2, Counter64 objects do not exist for v1 (Get and walk, RFC 3584), a v1 walk returns every other object, CPU temperature; ambient temperature when the sensor answers, and without it noSuchInstance (v2c Get), noSuchName (v1 Get) and skipped by GetNext, GetBulk and walks
- `test_snmp_agent` SNMPv2-MIB: sysORTable rows, snmp group counts (snmpInPkts includes the current request, snmpInTotalReqVars skips errors and exceptions, trap and response PDUs, silent drops, traps in snmpOutPkts), snmpSetSerialNo read-only
- `test_snmp_agent` GetBulk: a walk returns the whole MIB then endOfMibView, replies are capped at 16 varbinds (no amplification), a GetBulk does not make its source trusted

### `test/native/test_web_ui`: the web page (`lib/web_ui`)
- SHA-256 against the NIST vectors (incl. one million 'a'), split at every position around the padding boundaries
- Password rules, salted iterated hash compared with an independent Python `hashlib` result, the stepwise check equals the one-shot check for every step size and always runs all rounds, wrong/malformed hashes never match, default password alphabet, constant-time compare
- Sessions: token format, idle expiry, millis wrap, replacement of the least recently used, logout/clear; login throttle: lockout after 5 failures for 30 s, reset on success, wrap
- HTTP parser: GET/POST, every prefix incomplete, byte-by-byte, header lookup, size limits at the boundary, malformed request lines, chunked, NUL bytes, extra bytes; form and cookie decoding; HTML escaping
- Stylesheet: served without login with gzip and year-long caching headers, body straight from flash; the embedded data is a gzip file whose trailer matches the CSS size; pages link it; the policy allows it and still no scripts; mains and link badges follow the state
- Page logic with a fake backend: login form, right/wrong login, lockout, status values escaped, every action needs session and CSRF token, community, network and IPv6 validation (IPv6 stored in short form, empty removes it), save failure, reboot, logout, idle expiry, 404/405/400/413, security headers and Content-Length on every response, every single-byte corruption of two real requests gives a well-formed response

### `test/native/test_snmp_trap`: trap encoding
- mainsPowerLost and coldStart byte-for-byte identical to net-snmp's `snmptrap` output for the same fields; authenticationFailure differs only in generic-trap
- mainsPowerRestored fields, varbinds are the MIB objects with the right types, configured community used
- Failure on small buffers, missing community, too many varbinds
- Throttle: one per interval, first event at time 0, millis() wrap-around
- v2c traps end with `snmpTrapAddress.0` (left out without an IPv4 address); mainsPowerLostTooLong (v1 and v2c, byte-exact against net-snmp); `trapCanLeave` waits for the agent, the link and a destination, and an IPv4 or IPv6 destination needs an address of its own family

### `test/native/test_access_control`: rate limiter and access log
- Burst then refusal, refill at the configured rate, refill capped at the burst, sustained rate over 10 s within 2 requests of the theory
- Independent per-source buckets; full table evicts the least recently seen source; wrap-around; an idle gap of more than 2^31 ms refills instead of freezing
- Global bucket: a new source address on every request is capped at global burst + rate; a source refused by its own bucket does not drain the global one; a single source is still limited by its own bucket; off by default
- Trusted sources: not starved when spoofed sources drain the global bucket, not evicted by thousands of rotating sources, untrusted entries evicted first; marking an unknown source is ignored
- Trust is bounded: trusted sources together capped by their own shared bucket, at most `maxTrusted` trusted (a freed slot can be reused), trust expires after inactivity even for sources that never send again (their slots and eviction protection are released), `clearTrust()` revokes all; default without `configureTrusted()` unchanged
- Log newest-first, folding of repeats with first/last time and count, new entry on a different source or event, ring buffer keeps the newest 16, clamped index
- Event names, IPv4 formatting

### `test/native/test_tmp1075`: TMP1075 conversion and read state (`lib/tmp1075`)
- Conversion of the temperature register: values read from the carrier's sensor, 0, rounding (half away from zero), negative values, the datasheet's -40/-25/125 °C and the register's limits
- Read state: invalid before the first reading, valid after one, last value kept through two failed reads, invalid after three in a row, a good read restarts the count, recovery, read errors counted

### `test/native/test_cpu_load`: processor load (`lib/cpu_load`)

- CpuLoad: 0 before the first second, half busy reads 50, one-minute average (a busy half minute leaves after a minute), never above 100, timer wrap
- LoopLoad (core 0): an idle loop reads 0, also with the jittery pass times measured on the device (58-150 us, now and then 1.5 ms), one 500 ms pass raises the second it ends in, sustained long passes read high (also after five minutes of load), the idle reference leaves after 15 minutes, timer wrap

### `test/hil/test_host_resources.py`: HOST-RESOURCES-MIB and ENTITY-MIB on the device

- hrStorage RAM and LittleFS against the status screen, both processors described, entPhysicalSerialNum and entPhysicalFirmwareRev against the status screen and sysDescr
- CPU load: both cores at most 5 % after an idle minute (equal to the status screen), then core 0 at least 10 % after a minute of SNMP walks and web page loads (87 % measured), core 1 still at most 5 %

### `test/native/test_reset_button`: factory reset button logic
- Idle button: no events, LED off
- 10 s hold triggers exactly once, counted from the debounced press, within DEBOUNCE_MS of the physical press
- Short press and release 1 ms before the hold time do not trigger (raw level is checked, not only the debounced one)
- Contact bounce filtered; button held at boot must be released before it can trigger
- LED pattern: slow blink while held, fast after trigger, also after release
- millis() wrap-around, configurable hold time
- A released press reports how long it lasted (`lastPressMs()`), for the serial log

### `test/native/test_mains_alarm`: the 60 s mains alarm (`lib/power_monitor`)
- Not raised before 60 s, raised at 60 s; a restore clears the alarm and a pending trap; a restore at the mark raises nothing
- Boot without mains counts from boot; a quick restore and loss restarts the count; the pending trap is sent once; a new outage gets a new trap
- Stays raised across a `millis()` wrap, and the 60 s are timed across one

### `test/native/test_watchdog_logic`: core 1 heartbeat and restart count (`lib/watchdog_logic`)
- The heartbeat goes stale after exactly the stale time and recovers; counter and `millis()` wrap
- The restart record counts watchdog restarts, starts over at power-on, does not trust corrupted or random RAM, saturates; cause names

### `test/native/test_power_debouncer`: `lib/power_monitor`
- Initial state not counted as a change, stable input never changes
- Change accepted exactly after the debounce time, detection within the 10 ms requirement
- Glitches and contact bounce ignored, the timer restarts on every bounce
- Change counting, `millis()` wrap-around, delayed (sparse) samples

### `test/embedded/test_mains_input`: `lib/mains_input` on core 1
Works with or without the mains circuit connected.
- Core 1 publishes a snapshot and samples at about 1 kHz
- 200 000 reads from core 0 while core 1 writes: counters never go backwards
- Debounced state follows a stable pin
- Diagnostic (`-v`): raw toggle rate and duty cycle of GPIO27, to spot an unfiltered AC signal

### `test/embedded/test_settings_store`: `lib/settings` on LittleFS
Uses `test/settings_a.json` and `test/settings_b.json`, never the real settings.
- Save and reload, slot alternation, newest copy wins
- A half-written newest copy falls back to the previous one, and the next save overwrites the corrupt copy
- Both copies corrupt, or a file that is too large: defaults are used
- A v1 file is loaded and replaced by a newer copy
- Saving factory defaults (sequence 0) still beats the stored copy
- A maximum-size file (every text field at 63/31 characters, fully JSON-escaped) saves and reloads
- Files left by older firmware (`config.bak`, `config.schema.json`, `data/active_file`) are removed and the settings copies kept
- Stack use of save + load, measured by painting the stack: must stay within 1536 bytes (it was over 3 KB before the buffers were made static and -Os was restored, which crashed the device)
- The update server settings (`test/update_a.json`, `test/update_b.json`): newest copy wins, a torn copy falls back, saving them leaves the main settings file byte for byte unchanged, the factory reset's clear removes both copies (the key is not left in flash), and their save + load also stays within 1536 bytes of stack

### `test/embedded/test_ethernet`: W5500 and lwIP
- W5500 detected over SPI, link up, DHCP lease with a gateway, gateway answers ping

### `test/embedded/test_fw_flash`: `src/fw_update_service.cpp` on the real flash
Writes only the slot that is not running (left erased afterwards) and the two boot record sectors (saved and
restored). `test_build_src` is off, so the test compiles the service, `boot_info.cpp` and `stack_watch.cpp` in.
- The slot writer erases sectors and programs 256-byte pages of the other slot (read back through XIP)
- It refuses the running slot, offsets at or past the slot size or off a sector (erase) or page (program)
  boundary, and source data in flash
- Boot record writes go to the copy that is not the newest, with the sequence + 1
- The golden dev-signed `.upd` (`test/native/test_fw_receiver/golden_upd.h`) through the upload path in
  512-byte pieces, including the Ed25519 check from flash: verified, recorded as the trial slot, and core 0
  keeps more than 1024 bytes of stack never used (prints the figure; ~5.7 KB)
- A file with one image byte flipped is rejected with "bad signature", the other slot reads empty, no record
- After a rollback record the failed release is erased at start-up (the other slot reads empty, no switch,
  no second erase at the next start); after a kept record the other slot is left alone

### `test/hil/test_serial_menu.py`: the flashed firmware
- Menu text, status output with state, lease and MAC, ping from the PC
- Boot does not wait for DHCP: the prompt appears within 5 s, before `Network up`, and the lease still arrives
- Unknown input, backspace editing
- Each invalid entry rejected with one clear message; cancel leaves settings unchanged
- DHCP save → reboot → reconnect → ping
- Static IP applied after reboot, then DHCP restored (opt-in)
- Mains power line on the status screen
- SNMP settings on the status screen; option 4 rejects an empty or 32-character community, invalid hostnames (33 characters, spaces, dots, `_`, a leading `-`) and a line longer than the input buffer; Enter keeps, `-` clears; cancel changes nothing
- Mains loss and restore reported in the log and status (needs a mains switch, see above)

### `test/hil/test_snmp.py`: the SNMP agent on the device
- `snmpget` of sysDescr, sysObjectID, several varbinds in order; sysUpTime advances ~100 ticks per second
- Mains objects present and consistent with the serial status screen
- `snmpwalk` of everything from `.1` (v1: 224 objects in order, 233 while the device has an LLDP neighbour, LLDP-MIB first; End of MIB; v1 does not see the eight Counter64 columns), the system group (7) and the enterprise subtree (6; 5 with `RFPOWER_NO_TMP1075`)
- IF-MIB: ifDescr, ifType, ifMtu, admin/oper status, ifName; ifSpeed, ifHighSpeed and ifPhysAddress match the status screen; 20 requests raise the 64-bit packet and octet counters; ifInOctets is the low half of ifHCInOctets; the receive drop counters match the status screen; Counter64 is noSuchName in v1
- cpuTemperature between 10 and 85 °C and within 2 °C of the status screen
- ambientTemperature between 5 and 50 °C, within 0.5 °C of the status screen and below the CPU temperature (skipped with `RFPOWER_NO_TMP1075=1`, for a module without the carrier board)
- noSuchName for unknown OIDs and past the end; `snmpset` rejected and nothing changed
- Wrong community and SNMPv3 get no reply and are counted on the status screen
- SNMP settings set from the menu show up in `snmpget` immediately (no reboot), the old community is refused, and they survive a reboot; the originals are restored afterwards
- Rate limit: requests as fast as replies come back for 3 s are answered at burst + 50/s (±15 %), the rest dropped, counted and logged; answers resume after a pause
- Access log lists bad community and bad version from this PC, newest first; SetRequests are logged
- The device must use community `public` for this suite

- v2c: get, exceptions instead of errors, walk of all 232 objects (241 with an LLDP neighbour) plus the IP address rows, which the walk tests leave out and `test_snmp_ip_addresses_match_status_screen` / `test_ipv6.py` `test_snmp_ipv6_addresses_match_status_screen` check against the status screen ending with endOfMibView, `snmpbulkwalk` returns the same objects as `snmpwalk`, `snmpbulkget -Cn1 -Cr3`, `-Cr1000` capped at 16 varbinds and reaching endOfMibView, Set notWritable, wrong community; SNMPv3 gets no reply and is counted; v1/v2c counters

- 30 requests: every reply echoes its request id, worst case under 100 ms
- 300 malformed packets in bursts: agent keeps answering; oversized packet ignored; 5 simultaneous requests all answered
- Mains loss/restore over SNMP: status, change counter and last-change time; a ~1 s outage between two reads still shows as two changes in mainsPowerChanges (needs a mains switch)
- A 30 s outage leaves mainsPowerAlarm at 0; an outage over 60 s raises it (not before 57 s; status screen `lost over 60 s`) until mains returns (needs a mains switch)

### `test/hil/test_web.py`: the web page on the device
- Login form without device data, security headers; wrong password refused and logged; login shows the device status
- 5 failed logins lock out even the right password for 30 s, logged as `web locked out`
- Actions need session and CSRF token; community change applies to SNMP at once (old community refused) and is restored; invalid community and invalid static address rejected without restart
- Restart button and DHCP save restart the device (sysUpTime restarts, default password kept); static IP from the web page and back (opt-in `RFPOWER_STATIC_IP`)
- Hostname from the web page: status screen, SNMP sysName and mDNS (`name.local` resolved by the PC) follow at once without restart; invalid names refused; restored afterwards
- Contact and location from the web page: status screen and SNMP sysContact/sysLocation follow at once; a 64-character contact is refused; restored afterwards. Native: a form without the fields (a page from 3.0.0) keeps the stored values; the longest page also fits with an error box
- Web interface off via serial option 8: port 80 closed, SNMP keeps working, off survives a restart, back on; the page turning itself off asks first, then closes port 80, and serial option 8 reopens it at once; three logins per round leave closed connections in TIME-WAIT on port 80, and turning the web interface off and on still opens the port within 3 s, three rounds in a row (`SO_REUSE`; without it the port stayed closed for about 2 minutes)
- Logo and favicon: `/favicon.svg` is the path from `logo/RF-canopy-RGB-Orange.svg`, logo in the navbar and on the login card
- New default password via serial option 7: `n` changes nothing; `y` gives a new 10-character password, logs browsers out, refuses the old one and keeps the new one across a restart; an own password stays in use
- Own password via serial option 6: masked echo, old sessions logged out, default refused, hashed check under 2 s, mismatch and short passwords change nothing, `-` restores the default
- The Bootstrap stylesheet downloaded from the device unpacks to exactly Bootstrap 5.3.3 (official integrity hash), without slowing SNMP
- With an own password set, SNMP stays under 100 ms while a login (right or wrong) hashes the password
- A client that never finishes its request does not slow SNMP and is dropped after 5 s; 30 page loads keep SNMP under 100 ms and leave > 1 KB of core 0 stack never used; oversized request gets 413
- Needs the device default web password (the suite skips otherwise)

### `test/hil/test_ipv6.py`: IPv6 on the device
- Link-local address from the MAC (EUI-64), SLAAC address from the router's prefix (skipped without an IPv6 router), ping6
- SNMP get over IPv6, a wrong community over IPv6 logged with the PC's full address, web page at `http://[address]/`, `name.local` resolves to IPv6, IPv4 still answers
- Four requests sent back to back over IPv4 and over IPv6 are all answered (they crashed the device before `LWIP_IPV6_FRAG=0`)
- No restart during the run (sysUpTime keeps rising): the first IPv6 build panicked seconds after boot
- A v2c trap reaches an IPv6 destination; the menu refuses v1 traps to an IPv6 destination and a link-local destination

### `test/hil/test_ipv6_static.py`: static IPv6 address
- Menu option 9 refuses link-local addresses, `/64` suffixes and gateways outside the /64, shows the address in short form, and `n` changes nothing
- Opt-in (`RFPOWER_STATIC_IPV6`): the address is set, the device restarts, the address passes duplicate detection and is marked `(static)` while the link-local and SLAAC addresses stay; ping6, SNMP and the web page answer on it; then it is removed again
- The device must start without a static IPv6 address (the suite skips otherwise)

### `test/native/test_lldp`: `lib/lldp`
- Built frame byte for byte (written out from IEEE 802.1AB and decoded by `tcpdump -vv`), with and without IPv4/IPv6 management addresses, buffer one byte too small, strings over 255 bytes
- Parsing a Junos-style frame (Port ID as ifIndex, interface name in Port Description, organisational TLVs skipped), a frame captured from a Juniper EX2200-C, and our own frame; other EtherTypes, short and null frames; every truncation, a TLV past the end, missing or misordered mandatory TLVs, a wrong TTL length, no End TLV (all clean under the sanitizers)
- Text cut to 64 characters, non-printable bytes as `?`
- Neighbour: expires after its TTL (also across a `millis()` wrap), TTL 0 and link down forget it, the latest frame wins; display text (port description for a local port ID, else the port ID; chassis ID without a system name; `none seen`; cut to the buffer)
- Send timing: nothing while the link is down, 1 s after link up (again after a flap), every 30 s, 1 s after a change, across a `millis()` wrap
- LLDP-MIB (`test_snmp_agent`): lldpLocPortTable, lldpRemTable with the EX2200-C neighbour in v1 and v2c (Junos port name served as interfaceName), walks and GetBulk from `1.0` with and without a neighbour, noSuchInstance for another row (also another time mark), noSuchObject for an unknown column, mib-2 walks unchanged; `scripts/host_agent_check.sh` checks the same tables with net-snmp's tools (numeric OIDs: WSL has no LLDP-MIB)

### `test/hil/test_lldp.py`: LLDP on the device (opt-in, see above)
- The status screen names the switch and port within 35 s (`RFPOWER_LLDP_SWITCH=1`)
- `snmpget` (v1 and v2c) of lldpRemSysName and lldpRemPortId gives the same switch and port as the status screen, lldpLocPortDesc names our port (`RFPOWER_LLDP_SWITCH=1`); `test_snmp.py`'s walks from the root expect the LLDP-MIB objects first, the remote row only while the status screen shows a switch
- The switch lists the device by MAC with its hostname, port description and IPv4 address, and the device shows the port the switch says it is on (`RFPOWER_LLDP_SWITCH_CONSOLE`)
- A new hostname reaches the switch within 5 s (fails with a firmware that sends only every 30 s); the old one is restored

### `test/hil/test_switch_port.py`: no link, no DHCP server (opt-in, see above)
- Port disabled: within 10 s the status screen shows `State: no Ethernet link, check cable` and the Ethernet line link down; port enabled again: connected and SNMP answers, without a reboot
- DHCP dropped at the port and the device rebooted: `State: link up, waiting for DHCP lease` and no address, unchanged for 20 s (no fallback address); DHCP allowed again: a lease within 90 s (lwIP retries at most every 60 s) and SNMP answers, without a reboot

### `test/hil/test_dhcp_trap.py`: trap destination from DHCP
- Choosing `dhcp` asks for option 224 in the renewal, the LAN's server still answers (same lease and IP, SNMP answers), the status screen reports the ACK; back to fixed
- A forged DHCPACK with option 224 (Kea's capture re-addressed to the device, wrong transaction id) sent from this PC is ignored and counted
- Opt-in `RFPOWER_DHCP_TRAP_TARGET=<address>`: with a server that sends option 224, the destination in use is that address; if it is this PC, an authenticationFailure trap arrives there

### `test/hil/test_snmp_traps.py`: traps on the device (every test runs with v1 and with v2c traps)
- coldStart arrives after a reboot (empty ARP table, the trap is the first packet to this PC), exactly once, from the device's address, with a small time-stamp
- A wrong community sends one authenticationFailure trap; five more within 10 s send none but are counted in the access log
- No traps when the destination is off; trap counter on the status screen; SNMP keeps answering
- mainsPowerLost and mainsPowerRestored with real mains: correct version, source, varbinds, change counter and time-stamps (needs a mains switch)
- v2c traps have increasing request-ids; mainsPowerLostTooLong arrives within 75 s of a loss (needs a mains switch)

### `test/native/test_dhcp6_message`, `test_dhcp6_frame`, `test_dhcp6_client`: `lib/dhcp6_client`
- Real frames (`test/native/dhcp6_frames/frames.h`, made by [scripts/dhcp6_capture](../scripts/dhcp6_capture/README.md)): Kea 2.4.1's Advertise, Replies and NoAddrsAvail, dhcpcd 10's Solicit/Request/Renew, router advertisements with and without M, the device's own Information-request
- Messages: built Solicit byte for byte, the same Client ID, IA_NA and FQDN encoding as dhcpcd; Advertise/Reply read for this DUID only, IAID 1 only, addresses with preferred > valid or valid 0 dropped; every truncation and single-byte corruption
- Frames: headers and checksum identical to the device's and dhcpcd's frames; replies only for this MAC, ports 547 -> 546, valid checksum, no extension headers; router advertisements only with hop limit 255 from a link-local source
- Client: first Solicit within 1 s, best preference wins (255 at once), Request gives up after 10, Renew at T1 and Rebind at T2 (half and 80 % of preferred when 0), lease extended by a Renew reply, address removed at the end of the valid lifetime, infinite lifetimes never expire, a duplicate is declined, wrong transaction id or server ignored, Solicit backs off to 3600 s across a `millis()` wrap

### `test/hil/test_dhcp6_testnet.py`, `test_dhcp6.py`: DHCPv6 on the device (opt-in, `RFPOWER_DHCP6_TEST=1`)
- The test network works with any firmware: an O-flag advertisement makes lwIP's stateless client send an Information-request, Kea replies, and the device's SLAAC address is unchanged
- With the M flag the device gets an address from Kea's pool, marked `(dhcpv6)`, that answers ping6 and SNMP; the lease is renewed; the address is removed when Kea stops; an address this PC already uses is declined; core 0 keeps > 1 KB of stack; this PC's IPv6 default routes are unchanged
- Each test reboots the device first (option 2, DHCP unchanged); needs community `public`

### `test/hil/test_watchdog.py`: the hardware watchdog
- The hidden menu commands `stall0` and `stall1` hang core 0 (interrupts off) or core 1 for good; the device restarts within 8 s (core 1: 9 s), and the status screen shows `Last restart: watchdog` with the watchdog restart count one higher The core 1 restart time is taken from when the device stops answering SNMP (core 0 answers until the restart): Windows sometimes reports the USB drop 2 s late, which made the old USB-based timing fail now and then (9.1 s or 11.1 s against an 11 s limit). The core 0 time still comes from the USB drop, as core 0 stops answering at once.
- A menu reboot (option 2, DHCP unchanged) shows `Last restart: reboot` and does not count; twice the watchdog timeout of normal running restarts nothing
- Each test restarts the device once; settings are not touched (the reboot test skips in static mode)
- Three crash restarts in a row make the boot program start the other slot. So a stall test whose device last restarted by the watchdog first restarts it through option 2 (a planned restart clears the crash count): the tests never cause two crashes in a row (in static mode such a stall test skips)

### `test/hil/test_factory_reset.py`: the reset button (manual, `RFPOWER_MANUAL_BUTTON=1`)
- A ~3 s press is released without reset, the log shows how long it was held, and SNMP keeps answering while it is held
- A 10 s hold resets after 10000-10100 ms (SNMP keeps answering with the community set before the hold), defaults are saved in under 2 s, the device reboots with DHCP, `public`, default sysName, no trap destination and default stored static IP
- Your previous SNMP settings and stored static IP are restored afterwards

## Adding a test

1. Decide the layer. If the code can run without the Pico, move it into a pure library under `lib/` and test it in `test/native/`. Use `test/embedded/` only for flash, SPI, GPIO or timing, and `test/hil/` for end-to-end behaviour.
2. Add the test to the matching suite, or create a new suite folder named `test_<area>` with a `test_main.cpp`:
   - native: `int main()` with `UNITY_BEGIN()`, `RUN_TEST(...)`, `return UNITY_END();`
   - embedded: the same inside `setup()`, after `watchdog_disable()` (the boot program starts every image with the watchdog armed) and waiting up to 10 s for `Serial`, and an empty `loop()`
3. Name it `test_<what>_<expected behaviour>` and assert on the code under test.
4. Check it can fail: break the code briefly (or write the test before the fix) and watch it go red.
5. Run all native and embedded tests before committing.

## Diagnosing a hang or crash on the device

Every build has a hardware watchdog (8 s, `src/watchdog.cpp`): the boot program (`boot/`) arms it before it starts the firmware, `setup()` feeds it, and from the end of `setup()` a hang in `loop()` or on core 1 restarts the device. The status screen then shows `Last restart: watchdog`; the restart cause comes from the boot program (watchdog scratch 0, `src/boot_info.cpp`). It does not say where it hung. The diagnosis build tells you where:

```powershell
$env:PLATFORMIO_BUILD_FLAGS = '-DRFPOWER_DIAG -Wl,--wrap=panic'
& $pio run -e pico -t upload
Remove-Item Env:PLATFORMIO_BUILD_FLAGS
```

It shortens the watchdog early in `setup()` (4 s, the normal 8 s from the end of `setup()`), writes the step of `loop()` and of the frame hooks into RAM that survives the restart (not the watchdog scratch registers: the boot program uses scratch 0-2), records the program counter of a hard fault and the message of a `panic()` (lwIP asserts end there), and prints all of it after the watchdog restart: `DIAG: previous run PANICKED (called from 0x10025129): lwip`. Look the address up with `arm-none-eabi-addr2line -f -e .pio\build\pico\firmware.elf <address>`. This found the IPv6 timer-pool panic in `sys_timeout_abs` and the hard fault in `pbuf_free` caused by the core's UDP helper buffer.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `g++` not found / native build fails immediately | MinGW not installed or `PATH` not refreshed. Restart VS Code. |
| `could not open port 'COM16'` | Port in use (PuTTY, serial monitor) or Pico unplugged. |
| `No accessible RP2040 devices in BOOTSEL mode` | WinUSB driver missing on the boot interface, see Zadig above. |
| Embedded test hangs with no output | Board not running the test program. Press reset or replug. `scripts/wait_for_port.py` already waits for the port to come back after flashing. |
| `test_dhcp_lease_obtained` fails | No cable, no link LED, or no DHCP server on that network. |
| HIL tests skipped with "device is in Static mode" | Switch to DHCP with menu option 2 first. |
| Device stops answering on USB right after saving | Stack overflow on core 0 (8 KB since 2.3.0, 4 KB before). Run `test_settings_store` for the stack measurement, check no large buffer was put on the stack, and that `-Os` is not unflagged. Replug USB to recover. |
| A HIL run hangs and COM16 stays "Access denied" | The device hung and the test process is stuck in the USB driver; replug the Pico. |
| Trap tests: "no ... trap" although the status screen counts them as sent | Windows Firewall blocks inbound UDP for PlatformIO's Python; allow it (Public or Private profile, whichever the LAN is). |
| SNMP tests fail with timeouts right after the flood or rate-limit test | The rate limiter is still refilling; the tests wait 2.5 s, increase if your PC is slow. |
