# Time, syslog and update-site SNMP objects: design

Date: 2026-11-07. Status: agreed in conversation, reviewed by a builder check, to be planned.

## Why

The device only knows time since boot. After a power cut you cannot line its
access log or mains events up with anything else, and what it saw before a
reboot is gone. The device should know the real time (NTP) and send what
happens to a syslog server as it happens, so there is a dated history kept
off the device. LibreNMS should also be able to alert when the device stops
reaching its update site.

## Goals

1. **Real time from NTP (SNTP).** The device keeps UTC from an NTP server.
2. **Syslog.** Events go to a syslog server the user chooses (their own in the
   field; LibreNMS for testing here).
3. **Servers from DHCP or set by hand.** Both the NTP and the syslog server come
   from DHCP when the network offers them, or are typed in.
4. **SNMP objects for the update-site state**, with a LibreNMS sensor that warns
   when check-ins fail.

## Non-goals

- No RTC chip, no battery-backed time: time is lost at power-off and comes
  back with the first NTP answer.
- No syslog over TCP or TLS; no storing events on the device's flash for later.
- No NTP server on the device, no authenticated NTP.
- No SNMP SET (the agent stays read-only).

## Anti-goals (would make this a failure even if it works)

- Passwords, keys or the SNMP community in a syslog message.
- A flood of bad SNMP requests turning into a flood of syslog messages.
- SNMP answering slower than 100 ms, or the firmware's share of mains
  detection exceeding 10 ms, because of NTP or syslog work (`.clinerules`).
- Static RAM over 60 % (limit raised from 50 % by the user on 2026-11-07; it
  is 49.6 % today), or a buffer over
  ~256 bytes on core 0's stack.
- A settings change that a rollback to 3.5.1 cannot load (`.clinerules`
  Configuration Storage).
- A device that misbehaves before it has the time: everything must work as
  today until the first NTP answer.

## Decided (in conversation)

### Server sources
- **NTP server:** DHCP option 42 when offered; DHCPv6 option 56 (NTP server)
  too. Or typed in by hand: an IPv4/IPv6 address **or a DNS name** such as
  `pool.ntp.org` (the user's choice, for networks whose DHCP offers no NTP
  server). There is no built-in fallback: with `dhcp` and nothing offered, the
  device has no time, so it never contacts the internet unless told to.
- **Syslog server:** DHCP option 7 (log server), or typed in. IPv4 or IPv6
  address, optional port (default UDP 514).
- Each is set as **an address (NTP also a name), `dhcp` or `none`**, the same pattern as the trap
  destination (option 224) and the update site (option 225). A typed address
  wins over DHCP. Values from DHCP are used only from an answer the device
  accepted (as for 224/225).
- **Factory reset:** both go back to `dhcp` (DHCP is the default network
  configuration in the project rules). Claude's call, agreed.

### Where they are set
- Serial menu: new option **11. Time and syslog** (NTP server, syslog server).
  Status screen lines for both, showing the source (typed / from DHCP / none)
  and state (time synced or not, last sync; syslog destination).
- Web page: the same two fields, on the page that holds the SNMP settings.

### Time
- The device keeps UTC. Until the first NTP answer, everything works as now
  and times show as uptime.
- Shown on the status screen, access log and web page as **Danish local time**
  (CET/CEST, EU summer-time rule, switching automatically) with the zone
  written, e.g. `2026-11-07 19:42:10 CET`. Fixed rule, no time-zone setting.
  Claude's call (the user's answer was ambiguous).
- Syslog messages carry UTC with the zone marked.
- NTP poll every 15 minutes, sooner after a failure.
- SNMP: HOST-RESOURCES-MIB `hrSystemDate` (today left out because the device
  has no clock), served once the time is known; before that, as today.

### Syslog
- RFC 5424 format over UDP (port 514 unless the setting gives one).
- Hostname = the device hostname (sysName).
- Events, with severity:
  - **Mains:** power lost (warning), restored (notice), lost over 60 s, the
    alarm (critical).
  - **Restarts:** at boot, the reason (normal, watchdog/crash, firmware trial,
    rollback) and the firmware version.
  - **Firmware:** update installed and on trial, kept, rolled back (error).
  - **Web:** login OK, login failed and lockout.
  - **Settings changed** (web page or serial menu): what changed, never
    passwords or keys.
  - **SNMP:** wrong community and rate limiting, from the access log,
    throttled so a flood of bad requests cannot flood syslog.
  - **Network:** link up/down, new DHCP address, LLDP switch/port changed.
  - **Update site:** check-in failing, and OK again.
  - Ordinary SNMP polls are not sent.
- Events before the network and the time are there (boot reason, an early
  mains loss) wait in a small queue (about the last 16) with the uptime they
  happened at. They go out once there is an address, a server **and the
  time**, dated back from their uptime. If no time arrives within 2 minutes of
  having a server, they go out undated (RFC 5424 `-` timestamp) with the
  uptime in the text. When the queue is full the oldest is dropped and a
  "n events dropped" line is sent. Claude's call, agreed.
- Events are queued as text (16 entries of ~105 bytes, ~1.7 KB static RAM).
  Claude's call: first a compact ~12-byte form was agreed under the 50 % RAM
  limit; with the limit at 60 % the simpler text queue was chosen in the plan.
- "Settings changed" is sent for changes from the web page **and** the serial
  menu. Claude's call, agreed.
- Test destination: LibreNMS here (`librenms/test`, its syslog receiver
  switched on in the Docker setup); in the field, the user's own server.

### SNMP: update-site state
- New objects in the enterprise MIB (`RF-POWER-MONITOR-MIB`, next free group
  under 63050): last check-in result, minutes since the last good check-in,
  check-ins failed in a row. Documented in `docs/MIB.md` and the MIB file.
- LibreNMS definitions (`librenms/`): a sensor that warns after one hour
  without a good check-in.

## Constraints

- Project rules in `.clinerules` apply: logic in pure libraries under `lib/`
  with native tests (formatting syslog messages, the local-time rule, option
  parsing, event throttling, the settings codec); thin hardware layers in
  `src/`; tests with every change, truncated/corrupt input for anything parsed
  from the network or flash, sanitizers, HIL for visible behaviour.
- RAM: static RAM may grow to 60 % (user, 2026-11-07; 49.6 % today, so
  about 27 KB of room). The compact queue is static; message text buffers
  static or heap only while used; a DNS name for NTP costs lwIP DNS state.
  The plan shows the figure.
- Settings storage: the new server settings must stay loadable by 3.5.1 after
  a save by the new code (own A/B files like `update_settings_store.h`, or a
  JSON change older firmware ignores, with a test).
- Core 0 does network work; nothing may block it (SNMP within 100 ms).
- LibreNMS here holds the user's data: never `docker compose down -v` in
  `librenms/test`.

## Facts found in the code (for the builder)

- The settings decoder ignores unknown JSON fields, so 3.5.1 still loads files
  saved by the new code (rollback-safe if the fields go in the main file).
- Check-ins run every 30 minutes with a retry 5 minutes after a failure, so
  "warn after one hour without a good check-in" fits.
- Boot info already tells crashes, trials and rollbacks apart
  (`src/boot_info.cpp`, `src/fw_update_service.cpp`).
- The new enterprise group is 63050.8 (fwUpdate is .7).

- The Arduino-Pico core already builds lwIP's SNTP client
  (`lwip/apps/sntp.h`), sets the system time through
  `SNTP_SET_SYSTEM_TIME_US` → `__setSystemTime`, allows 2 servers and DNS
  names, and restarts SNTP on every netif status change
  (`LwipIntfDev::netif_status_callback`). `SNTP_GET_SERVERS_FROM_DHCP` is not
  set. lwIP options can be set from `platformio.ini` or `include/lwipopts.h`
  (CLAUDE.md Build notes).
- DHCP options 224/225 are requested and read by editing/reading frames in
  `src/net_stats.cpp` (`lib/dhcp_option`), only from ACKs lwIP accepted.
  Option 7 is not handled by lwIP.
- DHCPv6 runs in `lib/dhcp6_client` / `src/dhcp6_service.cpp` (our own
  stateful client).
- `lib/snmp_agent/src/snmp_mib.cpp` notes `hrSystemDate` left out for lack of
  a clock. Enterprise groups 63050.0-.7 are in use.
- The menu has options 1-10; settings are `SETTINGS_VERSION = 1`.

## Left to the builder

- Whether to use lwIP's SNTP client (with DHCP option 42 via
  `SNTP_GET_SERVERS_FROM_DHCP` or our own option reading) or a small own
  client; how DHCPv6 option 56 is read.
- Storage layout of the new settings (within the rollback rule above).
- Exact syslog message texts, MSGID/structured data, and the throttle numbers.
- Exact MIB object names, OIDs and types; the LibreNMS sensor details.
- Web page layout of the two fields.
- Version number (a minor release, 3.6.0, is expected).
- How the HIL tests reach an NTP server (public pool or the router) and
  receive syslog (LibreNMS or a listener in the test); anything needing a
  specific network resource is opt-in by environment variable.
