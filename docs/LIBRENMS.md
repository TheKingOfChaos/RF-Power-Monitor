# LibreNMS

The device works in LibreNMS as a plain SNMP device out of the box: it answers SNMPv2c, has the system group and the standard IF-MIB, so LibreNMS discovers it, graphs the Ethernet port's traffic and shows its uptime. To also get the mains power state and the CPU and ambient temperature as sensors, and the device recognised as an RF-UPS-Monitor with the RF logo, add the definitions in [librenms/](../librenms/) to your LibreNMS installation.

> **Status:** tested with LibreNMS 26.9.1 (Docker, [librenms/test/compose.yml](../librenms/test/compose.yml)) against the real device running firmware 1.0.1: OS, version and hardware detected, all three sensors created with the right values and limits, the port polled with traffic rates, and a mains trap logged. The RF logo as device icon and device page logo was checked with LibreNMS (Docker, `latest`) on 2026-10-31 against the device. The definition files also validate against LibreNMS's JSON schemas.

## What you get

| In LibreNMS | From |
|---|---|
| OS "RF-UPS-Monitor", type power, version (e.g. `1.0.2`) and hardware (`RP2040, W5500`) | sysObjectID `1.3.6.1.4.1.63050`, sysDescr |
| Port `eth0` with traffic, packets, speed and status | IF-MIB (64-bit counters over v2c) |
| State sensor "Mains power": present (OK) / lost (critical) | `mainsPowerStatus` |
| Temperature sensor "CPU (RP2040 die)", warning at 60 °C, critical at 70 °C, low limit −20 °C | `cpuTemperature` / 10 |
| Temperature sensor "Ambient (TMP1075)", warning at 40 °C, critical at 50 °C, low limit −20 °C; only with the carrier board | `ambientTemperature` / 10 |
| Count sensor "Mains power changes since boot" | `mainsPowerChanges` |
| Uptime, reboot detection | sysUpTime |

The temperature sensors set their low limit explicitly to −20 °C (the RP2040's minimum rating): without it LibreNMS sets a low limit 10 °C below the first reading (29.4 °C in the test), which alarms as soon as the room cools down. The CPU sensor keeps the index `boardTemperature.0` from before the MIB object was renamed `cpuTemperature`, so an existing installation keeps its graph after updating the definitions; replace the MIB file in LibreNMS together with the YAML files. The ambient limits suit a room with equipment; change them per device in LibreNMS if the box sits somewhere warmer.

A mains outage turns the state sensor critical (generic state 2). To be alerted, add the "State Sensor Critical" rule from LibreNMS's rule collection (Alerts → Alert Rules → Create rule from collection); a fresh installation has no alert rules. Tested with a real mains cut: the alert opened on the first poll after mains went off and recovered on the first poll after it came back. Polling runs every 5 minutes by default; a short outage between two polls still shows up as a step in "Mains power changes". For immediate notice, send the device's traps to LibreNMS's snmptrapd (below).

## Install

On the LibreNMS server, as the `librenms` user, with LibreNMS in `/opt/librenms`:

```bash
cd /opt/librenms
# OS detection and discovery
cp <this repo>/librenms/os_detection/rfupsmonitor.yaml resources/definitions/os_detection/
cp <this repo>/librenms/os_discovery/rfupsmonitor.yaml resources/definitions/os_discovery/
# MIB, in the directory named by mib_dir in the detection file
mkdir -p mibs/rfupsmonitor
cp <this repo>/mibs/RF-UPS-MONITOR-MIB.txt mibs/rfupsmonitor/RF-UPS-MONITOR-MIB
# RF logo: icon in device lists, logo on the device page
cp <this repo>/librenms/images/os/rfupsmonitor.svg html/images/os/
cp <this repo>/librenms/images/logos/rfupsmonitor.svg html/images/logos/
```

Then add the device with SNMP v2c and its community (menu option 4 on the device; change it from `public`):

```bash
./lnms device:add --v2c -c <community> <device ip>
```

or in the web interface: Devices → Add Device, SNMP version v2c. Use v2c, not v1: the 64-bit interface counters exist only in v2c.

If the device was added before the files were copied, rediscover it (device → Settings → Rediscover, or `./discovery.php -h <device ip>`) so it is detected as RF-UPS-Monitor.

LibreNMS's daily update and `validate.php` may complain about these extra files, since they are not part of the LibreNMS repository. Keep a copy, and copy them again if an update removes them.

### IPv6

The device answers SNMP over IPv6 too (see the README's IPv6 section). To poll it over IPv6, add it with the IPv6 transport:

```bash
./lnms device:add --v2c -c <community> -t udp6 <device IPv6 address>
```

or choose transport `udp6` in Devices → Add Device. Checked with LibreNMS 26.9.1 in the Docker setup below: added with `-t udp6`, the device is detected as RF-UPS-Monitor, and discovery and polling over IPv6 give the mains state, change counter, temperature and the `eth0` port. If LibreNMS already has the device by its IPv4 address, it refuses a second entry with the same sysName (`--force` skips that check). A static IPv6 address (serial menu option 9) gives the device an address that does not change with the router's prefix, which suits a monitoring system.

## Traps

Set the device's trap destination (menu option 4) to the LibreNMS server and the trap version to `2c` (or enter `dhcp` and let the DHCP server hand out the LibreNMS address in option 224, see [MIB.md](MIB.md#trap-destination-from-dhcp)), and set up snmptrapd on the server as described in the LibreNMS documentation (SNMP Trap Handler). LibreNMS has no trap handler for the RF-UPS-MONITOR-MIB notifications, so it logs them as unhandled traps: the device's event log shows an entry of type `trap` with the message `RF-UPS-MONITOR-MIB::mainsPowerLost` (or `...Restored`), checked with LibreNMS 26.9.1. The sensors change on the next poll.

LibreNMS finds the device for a trap by the address the trap came from. If something between the device and snmptrapd rewrites that address (NAT, such as Docker Desktop's port forwarding), LibreNMS logs "Could not find device for trap" in snmptrapd's output and drops the trap; the Docker setup below works around that.

## Trying it in Docker

[librenms/test/compose.yml](../librenms/test/compose.yml) starts a throw-away LibreNMS (MariaDB, Redis, LibreNMS, poller, trap receiver) with the definition files, the MIB and the logos mounted in, on http://localhost:8070:

```bash
cd librenms/test
docker compose up -d                     # wait about a minute for the database setup
docker compose exec -u librenms librenms ./lnms user:add -p <password> -r admin admin
docker compose exec -u librenms librenms ./lnms device:add --v2c -c public <device ip>
docker compose exec -u librenms librenms ./lnms device:discover <device ip>
docker compose exec -u librenms librenms ./lnms device:poll <device ip>
docker compose down -v                   # remove everything again
```

The poller polls every 5 minutes; traffic rates appear after the second poll. The device gets the RF icon at its first poll (LibreNMS stores the icon when it polls), so after updating the definitions on an existing installation the icon changes at the next poll.

### Traps in Docker

The `snmptrapd` container receives traps on UDP 162 of the PC. Set the device's trap destination to the PC's address (menu option 4); both trap versions work. Docker Desktop replaces the sender's address with its own (172.18.0.1), so LibreNMS alone could not find the device ("Could not find device for trap"). The device's traps also carry its address, v1 in agent-addr and v2c (from firmware 2.5.1) in `snmpTrapAddress.0`, which snmptrapd shows for both; [librenms/test/snmptrap-agent-addr.php](../librenms/test/snmptrap-agent-addr.php), mounted over LibreNMS's `snmptrap.php`, puts that address back before LibreNMS handles the trap. Without NAT in between, on a normal LibreNMS server, the script is not needed. Only traps with the community in `TRAP_COMMUNITY` (default `public`) are accepted, e.g. `TRAP_COMMUNITY=mycommunity docker compose up -d`.

**Alerts from traps:** LibreNMS only logs these traps; alerts come from the sensors. After a mainsPowerLost or mainsPowerRestored trap, the same script polls the device's sensors, which runs its alert rules, so "State Sensor Critical" opens when mains goes and recovers when it returns within seconds instead of at the next 5-minute poll. On a LibreNMS server without this script the alert comes with the next poll.

With WSL in mirrored networking mode (below), a firewall in front of WSL (the Hyper-V firewall) drops traps from the network until UDP 162 is allowed, once, in an administrator PowerShell:

```powershell
New-NetFirewallHyperVRule -Name "RFUPS-SNMP-Trap" -DisplayName "RF-UPS-Monitor SNMP traps (WSL/Docker)" -Direction Inbound -VMCreatorId '{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}' -Protocol UDP -LocalPorts 162 -Action Allow
```

Checked on 2026-10-31 with v2c traps: coldStart, authenticationFailure, mainsPowerLost and mainsPowerRestored were matched to the device, and the mains sensor changed 2 s after each mains trap. With v1 traps, a 30 s mains cut (Dirigera outlet) gave `RF-UPS-MONITOR-MIB::mainsPowerLost` and `...Restored` in the device's event log, and the "State Sensor Critical" alert opened 3.4 s after mains went off and recovered 3.4 s after it came back; and a wrong community an "SNMP Trap: Authentication Failure" entry. Traps a test sends from the PC itself (`snmptrap` in WSL) arrive without the firewall rule, so they do not show whether the device's traps get through.

To keep the setup running, a `compose.override.yml` next to `compose.yml` (loaded by `docker compose` on its own) can add `restart: unless-stopped` to each service and the community; `librenms/test/compose.override.yml` is gitignored for that.

### IPv6

The compose network has IPv6 (`enable_ipv6`, NATed by Docker), so the containers can poll the device over IPv6 (`./lnms device:add --v2c -c public -t udp6 <device IPv6 address>`). In WSL this needs mirrored networking (`networkingMode=mirrored` under `[wsl2]` in `%USERPROFILE%\.wslconfig`, then `wsl --shutdown`); with the default NAT networking WSL has no IPv6 route to the LAN. WSL stops when no terminal uses it, and the containers do not restart on their own: run `docker compose up -d` again after that.

## Files

| File | Purpose |
|---|---|
| [librenms/os_detection/rfupsmonitor.yaml](../librenms/os_detection/rfupsmonitor.yaml) | Matches sysObjectID `.1.3.6.1.4.1.63050`; OS name, type, icon, MIB directory, overview graphs |
| [librenms/os_discovery/rfupsmonitor.yaml](../librenms/os_discovery/rfupsmonitor.yaml) | Version and hardware from sysDescr; the state, temperature and count sensors |
| [mibs/RF-UPS-MONITOR-MIB.txt](../mibs/RF-UPS-MONITOR-MIB.txt) | Lets LibreNMS resolve the `RF-UPS-MONITOR-MIB::` names |
| [librenms/test/snmptrap-agent-addr.php](../librenms/test/snmptrap-agent-addr.php) | Docker setup only: matches traps to the device by the address in them (v1 agent-addr, v2c snmpTrapAddress.0), which Docker's port forwarding would otherwise hide, and polls the sensors after a mains trap so the alert follows within seconds |
| [librenms/images/os/rfupsmonitor.svg](../librenms/images/os/rfupsmonitor.svg), [librenms/images/logos/rfupsmonitor.svg](../librenms/images/logos/rfupsmonitor.svg) | The RF logo (from [logo/](../logo/)): square icon for device lists, wider logo for the device page. Named by `icon:` in the detection file; without them LibreNMS shows the generic icon |

If you change the MIB, keep the object names used in the discovery file. The temperature sensors' divisor (10) matches the MIB's units of 0.1 °C.
