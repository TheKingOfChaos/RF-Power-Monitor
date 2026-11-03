# RF-Power-Monitor

Raspberry Pi Pico (RP2040) with a W5500 Ethernet module: a mains power monitor read over SNMP v1/v2c (traps, IF-MIB counters, CPU and ambient temperature; LibreNMS definitions in `librenms/`), configured over a USB serial menu. The version is in `include/version.h`; record changes in `CHANGELOG.md`.

The project rules are in `.clinerules` and apply to all work here, including its testing rules:

@.clinerules

## Commands

`pio` is not on PATH: use `%USERPROFILE%\.platformio\penv\Scripts\pio.exe`.

| Task | Command |
|---|---|
| Build firmware | `pio run -e pico` |
| Flash firmware | `pio run -e pico -t upload` |
| Unit tests (PC) | `pio test -e native` |
| On-device tests | `pio test -e pico` (replaces the firmware, reflash afterwards) |
| HIL tests | `%USERPROFILE%\.platformio\penv\Scripts\python.exe -m unittest discover -s test/hil -v` |
| Sanitizers | `wsl -d Ubuntu-24.04 -- bash scripts/sanitize.sh` (after `pio test -e native`) |
| SNMP vs net-snmp, no hardware | `wsl -d Ubuntu-24.04 -- bash scripts/host_agent_check.sh` |
| SNMP HIL only | `... python.exe -m unittest discover -s test/hil -p "test_snmp.py" -v` |

Details and setup are in `docs/TESTING.md`.

## Build notes

- lwIP is compiled from source with the core's `lwipopts.h` plus our `build_flags`, so lwIP options can be set in `platformio.ini`; options the core's file sets without `#ifndef` (`LWIP_STATS`, for the MIB-II counters) are overridden in `include/lwipopts.h`, which `#include_next`s the core's (IPv6, `MEMP_NUM_SYS_TIMEOUT`, `LWIP_IPV6_FRAG=0`, `LWIP_IPV6_NUM_ADDRESSES=4`, `SO_REUSE=1` and the hook file `include/lwip_hooks.h` are; the comments there say why).
- Each core has its own 8 KB stack (`core1_separate_stack` in `src/main.cpp`); the status screen and a HIL test watch core 0's headroom.
- A firmware hang takes USB serial with it: use the `RFPOWER_DIAG` build (docs/TESTING.md) before asking the user for BOOTSEL.

## Working with the hardware

- Pico on `COM16`, 115200 baud. The firmware waits up to 3 s for a terminal, and needs DTR set to print.
- If upload or tests fail with "Access denied" on COM16, PuTTY is probably open. Ask the user to close it; do not kill it.
- snmpwalk/snmpget are in WSL: `wsl -d Ubuntu-24.04 -- snmpwalk -v1 -c public <ip>`.

## Layout

- `src/`: firmware entry (`main.cpp`), serial menu (`cli.cpp`), factory reset
- `include/pins.h`: all GPIO assignments
- `lib/netconfig`, `lib/settings_codec`: pure C++, host tested
- `lib/settings`: LittleFS A/B storage on top of `settings_codec`
- `lib/snmp_ber`: BER codec and OIDs (pure, host tested, sanitizer checked)
- `lib/snmp_agent`: SNMP v1/v2c messages, MIB table, request handling, traps (pure); `src/snmp_service.cpp` runs it on UDP 161. MIB docs in `docs/MIB.md`
- `lib/cpu_load` (pure): one-minute processor load per core (hrProcessorLoad); `src/host_stats.cpp` measures both cores, RAM, LittleFS and the serial number for HOST-RESOURCES-MIB and ENTITY-MIB
- `lib/if_stats` (pure): frame counting and PHY decoding; `src/net_stats.cpp` hooks the lwIP netif and reads the W5500 PHY; `src/cpu_temp.cpp` reads the RP2040 temperature; `lib/tmp1075` (pure) and `src/ambient_temp.cpp` read the carrier's TMP1075 on I2C0
- `lib/power_monitor` (pure: mains debouncer, 60 s mains alarm, reset button) and `lib/mains_input` (GPIO27 on core 1, `setup1()`/`loop1()` in `main.cpp`); `src/factory_reset.cpp` samples GPIO22 on core 1
- `lib/dhcp6_client` (pure): stateful DHCPv6 (one IA_NA, RFC 8415): messages, frames, router-advertisement flags, client state machine; `src/dhcp6_service.cpp` runs it, sending and receiving frames around lwIP (whose stateless DHCPv6 owns UDP 546) via the `src/net_stats.cpp` hooks. HIL test network in `test/hil/dhcp6/` (unicast RA + Kea in WSL as root, opt-in `RFPOWER_DHCP6_TEST=1`)
- `lib/watchdog_logic` (pure): core 1 heartbeat check and restart count; `src/watchdog.cpp` runs the hardware watchdog (8 s, fed by core 0 only while core 1 is alive; hidden menu commands `stall0`/`stall1` for its HIL test)
- `lib/access_control` (pure): SNMP rate limiter and access log, keyed by `SourceKey` (full IPv4 or IPv6 address, 128 bits; `src/source_key.h`)
- `lib/dhcp_option` (pure): asks for and reads DHCP option 224 (trap destination) by editing/reading frames in `src/net_stats.cpp`
- `lib/lldp` (pure): LLDP frames built and parsed, the one neighbour (TTL), send timing; `src/lldp_service.cpp` reads the switch's frames in the `src/net_stats.cpp` input hook, sends with `netStatsSendFrame()`, feeds the status screen, web page and LLDP-MIB (`lldpServiceSnmpState`) (hidden menu command `lldpdump`). HIL `test/hil/test_lldp.py` reads the switch's neighbour list over its serial console (`test/hil/switch_console.py`)
- `lib/web_ui` (pure): web page HTTP parser, login/sessions, pages; `src/web_service.cpp` serves it on TCP 80 (non-blocking); `src/stack_watch.cpp` core 0 stack high-water mark; `src/hostname.cpp` hostname (= sysName) for DHCP and mDNS; `src/ipv6_static.cpp` static IPv6 address and gateway (lwIP `LWIP_HOOK_ND6_GET_GW`)
- `hardware/pico-poe-mains-carrier/`: carrier PCB (KiCad 10) for the W5500-EVB-Pico-PoE; generated by its `tools/generate.py` (hand edits in KiCad are overwritten), its own README documents circuit and isolation
- `hardware/rf-power-monitor-rev2/`: rev 2 single board (RP2040 + W5500 on the board, WIZPoE-P1 plug-in PoE module; designed with Adom AI Flow, not built yet). Same pin map, firmware unchanged. Schematic generated from `design/netlist.json` by `kicad/tools/gen_sch.py`; the routed board is edited in KiCad, then `kicad/tools/fab.sh`. AI Flow's `aiflow-run/` stays out of git
- CI: `.github/workflows/tests.yml` runs native tests, sanitizers, net-snmp check and the firmware build
- `test/native`, `test/embedded`, `test/hil`: the three test layers
