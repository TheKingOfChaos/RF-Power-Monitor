# RF-Power-Monitor

A mains power monitor built on a Raspberry Pi Pico with a W5500 Ethernet module. It reports whether mains power is present over SNMP v1 and v2c, including traps, together with standard interface statistics (IF-MIB), the CPU temperature and the ambient temperature (TMP1075 on the carrier board), so it can be monitored by LibreNMS or any other SNMP manager. It is configured through a menu on the USB serial port or a web page.

Current version: **3.6.1** (see [CHANGELOG.md](CHANGELOG.md)).

## Overview

![System overview: mains detector, Pico with W5500, PC and network](docs/diagrams/system-overview.svg)

An isolated detector (optocoupler with filtering) turns mains presence into a clean 3.3 V level on GPIO27; in the test setup an IKEA smart plug in front of it lets the tests switch mains off and on. The Pico's core 1 samples GPIO27 and the reset button (GPIO22) every millisecond; core 0 runs the network, the SNMP agent, the web page and the serial menu. Settings live in the Pico's flash in two copies, and the W5500 provides 10/100 Ethernet over SPI. On the network the device gets its address by DHCP (or a static IP), is reachable as `RF-Power-XXXXXX.local` (mDNS; XXXXXX = the last 6 hex digits of its MAC), answers SNMP v1/v2c on UDP 161, sends traps on UDP 162, serves a configuration web page with login on TCP 80, sets its clock by NTP and sends its events to a syslog server, when the network has them. The USB serial port carries the configuration menu and the hardware-in-the-loop tests. More diagrams are in [docs/diagrams/](docs/diagrams/README.md).

## Status

| Feature | State |
|---|---|
| Ethernet with DHCP (default) or static IP | Working |
| Serial menu for all settings | Working |
| Settings stored in flash, power-cut safe | Working |
| Factory reset button (GPIO22, 10 s hold, LED feedback) | Working, tested with the real button |
| Mains power monitoring on GPIO27 (core 1, debounced, 60 s alarm) | Working, verified with real mains (automated with a smart plug) |
| SNMP v1 and v2c agent (Get, GetNext, GetBulk) with rate limiting and access log | Working, tested with net-snmp |
| SNMP traps in v1 or v2c (coldStart, authenticationFailure, mains, firmware) | Working, mains traps verified with real mains |
| Standard MIBs: SNMPv2-MIB, IF-MIB (64-bit), IP-MIB, MIB-II counters, HOST-RESOURCES-MIB, ENTITY-MIB, LLDP-MIB | Working |
| CPU temperature (RP2040) and ambient temperature (TMP1075) | Working |
| Web page with login (status and settings; can be turned off) | Working |
| Hostname for SNMP sysName, DHCP and mDNS (`name.local`) | Working |
| IPv6 dual stack: SLAAC, static address, DHCPv6 | Working |
| LLDP: the switch lists the device; the device shows the switch and port | Working |
| SNMP trap destination from DHCP option 224 | Working, tested with Kea 2.2.0 |
| LibreNMS OS definition | Working, tested with LibreNMS 26.10.2 |
| Signed firmware updates with rollback (3.3.0) and the [update site](update-server/README.md) (3.4.0) | Working, HIL-tested end to end |
| Time from NTP and syslog (RFC 5424) (3.6.0) | Working, tested with LibreNMS's syslog receiver |

See [TODO.md](TODO.md) for the detailed plan.

## Quick start

**Hardware.** A WIZnet W5500-EVB-Pico-PoE (or a Pico with a W5500 module on SPI0) on the carrier board, which holds the isolated mains detector, the TMP1075 and the factory reset button. Power comes over PoE or USB. Boards, wiring and pins: [docs/HARDWARE.md](docs/HARDWARE.md).

**Build and flash** (VS Code with PlatformIO; on Windows the WinUSB driver once, see [docs/TESTING.md](docs/TESTING.md#usb-driver-for-flashing)):

```powershell
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
& $pio run -e pico               # build
& $pio run -e pico -t upload     # build and flash (close PuTTY first)
```

If upload fails, hold BOOTSEL while plugging in the Pico and copy `.pio\build\pico\firmware_full.uf2` to the `RPI-RP2` drive. **Never put the plain `firmware.uf2` on a BOOTSEL drive:** it has no boot program and the device will not start. Use `firmware_full.uf2` or a release's `-usb.uf2`. Field releases, signing and the bench tool: [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) and [docs/SETUP_MANUAL.md](docs/SETUP_MANUAL.md).

**First login.** Open the USB serial port at 115200 baud (PuTTY, which sets DTR) and press Enter: menu option 1 shows the device's address and its default web password (`Web login: user admin, default password "..."`). Then open `http://<address>/` or `http://RF-Power-XXXXXX.local/` and log in as `admin`. Next steps: the [setup manual](docs/SETUP_MANUAL.md) to install a device, the [user manual](docs/USER_MANUAL.md) for daily use.

## Documentation

| Document | Contents |
|---|---|
| [docs/SETUP_MANUAL.md](docs/SETUP_MANUAL.md) | **Installing a device**: safety, assembly, first configuration, LibreNMS, proving the alarm works, update site, time and syslog, bench setup of a new board, firmware signing key, checklist |
| [docs/USER_MANUAL.md](docs/USER_MANUAL.md) | **Daily use**: LEDs, label, web page (incl. security and paths for scripts), what to do on an alarm, changing settings, the serial menu (every option and status line), factory reset, firmware update, troubleshooting |
| [docs/REFERENCE.md](docs/REFERENCE.md) | **How the device behaves, in detail**: network, LLDP, time and syslog, hostname, IPv6, mains monitoring, watchdog, factory reset, settings storage |
| [docs/HARDWARE.md](docs/HARDWARE.md) | What the firmware runs on, carrier board, rev 2 board, pin configuration |
| [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) | Building and flashing, tests in short, project structure |
| [docs/TESTING.md](docs/TESTING.md) | Test layers, how to run them, automated mains tests with a smart plug, what each suite covers |
| [docs/MIB.md](docs/MIB.md) | Every SNMP object (SNMPv2-MIB, IF-MIB, IP/ICMP/TCP/UDP, HOST-RESOURCES-MIB, ENTITY-MIB, LLDP-MIB, RF-POWER-MONITOR-MIB), traps, v1/v2c differences, rate limiting, errors, limits |
| [docs/LIBRENMS.md](docs/LIBRENMS.md) | Monitoring the device with LibreNMS: install, sensors, alerts, traps, syslog, Docker test setup |
| [update-server/README.md](update-server/README.md) | The update site (Docker): device list, releases, assignments, shared key |
| [hardware/pico-poe-mains-carrier/](hardware/pico-poe-mains-carrier/README.md) | Carrier PCB rev 1.1 (KiCad 10): circuit, isolation, mounting, fab files for JLCPCB, 3D models |
| [hardware/rf-power-monitor-rev2/](hardware/rf-power-monitor-rev2/README.md) | Rev 2 board, RP2040 and W5500 on the board (ordered 2026-11-07, not built yet): differences from rev 1, pins, isolation, known limits, fab files |
| [docs/diagrams/](docs/diagrams/README.md) | Excalidraw diagrams (editable) of the system, firmware, SNMP, mains flow, settings, tests and the update site |
| [CHANGELOG.md](CHANGELOG.md) | What changed in each version |
| [.clinerules](.clinerules) | Project and test rules |

## Safety

- The mains detection circuit must be galvanically isolated (optocoupler). The carrier board's isolation (clearance, creepage, slot) is described in its [README](hardware/pico-poe-mains-carrier/README.md#isolation).
- Mains wiring must be done by a qualified person, in a closed enclosure, with the supply fused upstream.
- No GPIO may see more than 3.3 V.
- Ground all modules properly.

## License

MIT

The web page includes Bootstrap's CSS, Copyright 2011-2024 The Bootstrap Authors, MIT license (https://github.com/twbs/bootstrap/blob/main/LICENSE).
