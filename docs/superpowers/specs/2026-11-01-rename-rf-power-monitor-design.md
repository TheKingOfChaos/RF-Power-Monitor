# Rename to RF.Power-Monitor, and bring docs and diagrams up to date

Date: 2026-11-01. Status: draft.

## Intent

The project is called RF-UPS-Monitor, but the device monitors mains power,
not a UPS. The user wants it renamed to **RF.Power-Monitor**, everywhere
("rename all"), and all docs and diagrams updated. LibreNMS here is only a
test install, so losing its history for the device is fine.

## Names (old → new)

| Where | Old | New |
|---|---|---|
| Display name (docs, diagrams, menu title, web page, boot banner, sysDescr, LLDP System Description, LibreNMS OS text, MIB descriptions) | RF-UPS-Monitor | RF.Power-Monitor |
| MIB module and file | RF-UPS-MONITOR-MIB, `mibs/RF-UPS-MONITOR-MIB.txt` | RF-POWER-MONITOR-MIB, `mibs/RF-POWER-MONITOR-MIB.txt` |
| MIB identifiers with the name (module identity, notification and group names) | `rfUpsMonitor...` | `rfPowerMonitor...` |
| LibreNMS OS name and files (`os_detection`, `os_discovery`, logos) | `rfupsmonitor` | `rfpowermonitor` |
| Default hostname / sysName (settings_codec default, factory reset) | `rf-ups-monitor` | `rf-power-monitor` |
| mDNS `_http` service instance | `RF-UPS-Monitor` | `RF.Power-Monitor` (see Risks) |
| Test environment variables | `RFUPS_*` | `RFPOWER_*` (our call: readable, no clash) |
| C/C++ identifiers with the name (e.g. lwIP hook `rfupsIPv6Gateway`) | `rfups...` | `rfpower...` |
| Carrier board title block and silkscreen (`hardware/.../tools/generate.py`, then regenerate) | `RF-UPS-Monitor carrier...`, `RF-UPS-Monitor v1.1` | `RF.Power-Monitor carrier...`, `RF.Power-Monitor v1.1` |

Unchanged on purpose: the enterprise number 63050 and every OID (monitoring
keeps working by number), object names that do not contain the project name
(`mainsPowerStatus`, ...), settings file names and JSON keys (stored
settings must keep loading), the hardware revision number.

## Goals

1. Every occurrence in the table is renamed in code, tests, scripts, docs,
   LibreNMS files, the MIB and the hardware generator. Afterwards
   `git grep -i -E "rf-?ups|rfups|ups-monitor"` finds only these allowed
   exceptions (our calls):
   - the CHANGELOG's history entries and the "Updating from 2.x" list, and
     the old specs and plans under `docs/superpowers/` (including this one);
   - repository URLs (`github.com/TheKingOfChaos/RF-UPS-Monitor...`), since
     the repository is not renamed;
   - nothing else. Dated test records in the docs ("Checked with LibreNMS
     ... detected as RF-UPS-Monitor") are re-run after the rename where the
     plan re-checks them (LibreNMS detection, sensors, traps, LLDP neighbour)
     and rewritten with the new name and date; records that are not re-run
     are reworded to say they were checked under the old name.
   - The captured DHCPDISCOVER in `test/native/test_dhcp_option/frames.h`
     carries the hostname `rf-ups-monitor` in its bytes: it is recaptured
     from the device after it has its new hostname (the same way as the
     original capture), so it stays a real capture.
   - The Hyper-V firewall rule `RFUPS-SNMP-Trap` documented in LIBRENMS.md:
     the docs give the new name (`RFPOWER-SNMP-Trap`, display name with
     RF.Power-Monitor); the user's existing rule keeps working under its old
     name (only a label), CLAUDE.local.md notes that. No admin step needed.
2. The user's own Pico gets the new hostname `rf-power-monitor` (menu
   option 4), since its stored sysName is the old default (our call, part of
   "rename all"). Its community `rfit` and trap settings stay.
3. The user's test LibreNMS (`librenms/test`, Docker) is moved over: new
   definitions, logos and MIB copied in, the old `rfupsmonitor` files and old
   MIB removed from the container, snmptrapd restarted (it loads MIBs at
   start), the trap script's regex updated, the device rediscovered so its OS
   becomes RF.Power-Monitor with its sensors and the LLDP neighbour. Never
   `docker compose down -v`.
4. The user's Windows user environment variables `RFUPS_*` (listed in
   CLAUDE.local.md) are recreated as `RFPOWER_*` with the same values and the
   old ones removed; CLAUDE.local.md updated. Token files and secrets are
   not read.
5. Docs brought up to date with the firmware (2.7.0 content): README, docs/*.md,
   CLAUDE.md, TODO.md, docs/LIBRENMS.md, hardware README. Check each section
   against the code, not only the name: LLDP and LLDP-MIB, the watchdog,
   DHCPv6, the mains alarm, the web row, test layers and counts.
6. All 8 diagrams regenerated (`docs/diagrams/make_diagrams.py` →
   `.excalidraw`, then `render_svg.sh` in Docker → `.svg`) with the new name
   and updated content: firmware architecture (`lldp_service`, `watchdog`,
   `dhcp6_service`, `lib/lldp`, `lib/watchdog_logic`, `lib/dhcp6_client`),
   system overview (the switch and LLDP), network addresses (DHCPv6), SNMP
   request flow (LLDP-MIB objects), test layers (LLDP and watchdog HIL,
   switch console, net-snmp check), mains flow (mains alarm after 60 s).
   Each diagram shows only what the code does.
7. Version **3.0.0** (major: the MIB module and object names, the LibreNMS
   OS name, the default hostname and the test variables change). CHANGELOG
   entry with an "Updating from 2.x" list: copy the new MIB and LibreNMS
   files, delete the old ones, rediscover, restart snmptrapd, optionally
   rename the device (hostname), rename `RFUPS_*` variables.

8. Commits on a branch; merging, pushing and a `v3.0.0` tag are decided by
   the user at the end, as for earlier work.

## Non-goals

- Renaming the GitHub repository or the local folder `RF-UPS-Monitor-v3`
  (outside the repo; the user can do it, then the `repository.url` in the
  `library.json` files follows). Links to the repository keep the current
  URL.
- Changing OIDs, the enterprise number, settings files or the board
  revision.
- Rewriting history: CHANGELOG entries for earlier versions and old specs and
  plans under `docs/superpowers/` keep the old name.

## Risks and checks

- **mDNS instance name with a dot.** "RF.Power-Monitor" as a DNS-SD service
  instance name is legal (RFC 6763 4.3) but lwIP's mdns must escape the dot;
  check that the web service still shows up (`dns-sd -B _http._tcp` or
  Avahi in WSL is not usable with NAT networking: check from Windows) and fall
  back to "RF Power Monitor" if lwIP sends it as two labels. The hostname
  itself has no dot (`rf-power-monitor.local`).
- **sysDescr length** changes; it is not length-limited, but LLDP's System
  Description TLV and the LLDP frame size must still fit (test_lldp).
- **Board files:** regenerating changes the KiCad files and fabrication
  outputs; boards already made keep the old print. The generator is the
  source of truth (hand edits in KiCad are overwritten); regenerate with the
  documented tool (`hardware/pico-poe-mains-carrier/README.md`), run its
  checks (DRC/ERC as documented) and re-export the fab files if the README
  says the fab outputs are generated.

## Tests

Every layer passes after the rename: native (incl. tests that assert the
name, e.g. sysDescr, menu title, default hostname, web page), sanitizers,
`host_agent_check.sh` (MIB loads without warnings under its new name),
on-device tests, HIL suites (with `RFPOWER_*` variables), and LibreNMS:
OS detected as RF.Power-Monitor, sensors present, LLDP neighbour, a trap
shown with the new MIB name after snmptrapd restart.

## Left to the builder

- Order of the work, how to batch renames (scripted replacements must be
  checked by reading the diff), exact wording of updated doc sections.
- Diagram layout details.
