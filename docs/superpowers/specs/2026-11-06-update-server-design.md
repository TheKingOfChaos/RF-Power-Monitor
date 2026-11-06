# Update server: device check-in and chosen firmware updates: design

## Intent

The user wants one place that knows every RF-Power-Monitor on the network and can update the ones they
pick. Today a device is found by its DHCP address (which changes) and updated one at a time from its web
page or with `tools/fw_update.py`. The new piece is a small website in Docker, next to LibreNMS on the
user's PC. Each device reports its address and hostname to it every 30 minutes. The user uploads a signed
release once, ticks the devices that should get it, and those devices download and install it at their
next check-in.

The device's existing update path (signed `.upd`, write to the slot that is not running, trial boot,
rollback) stays the only way firmware gets installed. The site only decides *which* devices fetch *which*
release; it never needs to be trusted with what firmware runs, because the device still checks the
Ed25519 signature.

## Goals

- **Check-in every 30 minutes.** A device with an update server URL configured sends a check-in shortly
  after its network comes up (*designer's call*: about 1 minute after it has an address, so a trial boot
  is confirmed quickly, see below) and then every 30 minutes. The check-in carries:
  - the RP2040 serial number (the device's identity on the site; the same one ENTITY-MIB reports);
  - hostname (= sysName);
  - IPv4 address, and the IPv6 addresses it has (*designer's call*: global and static ones; link-local optional);
  - MAC address, firmware version, running slot;
  - the state and last result of a firmware update (idle, downloading, on trial, kept, rolled back and
    why, download or verification failed and why).
- **Update server URL setting.** New setting, empty by default = check-ins off. Set from the serial menu
  and the web page settings, shown on the status screen and web page together with the last check-in
  result and time. Plain `http://host[:port][/path]`, host a name or an IPv4/IPv6 address (no HTTPS:
  the firmware has no TLS and the signature protects the firmware itself).
- **URL from DHCP** (the user's decision): like the trap destination with option 224, the URL can come
  from a DHCP option. *Designer's call*: option **225**, a text option holding the URL, with a setting
  "update URL from DHCP" the way `trapFromDhcp` works (the DHCP value replaces the stored URL while the
  setting is on). The same rule as option 224 applies: the value is only used from an ACK that lwIP
  accepted (see `src/net_stats.cpp`). `lib/dhcp_option` today reads only 4-byte options and must learn
  to read a text option (length 1..the URL maximum, rejected rather than truncated if longer).
- **The site lists devices.** Anyone on the LAN can open it without logging in and see one row per
  device (by serial number): hostname, IP addresses, firmware version, last check-in, online/offline,
  update status. *Designer's call*: **offline** after two missed check-ins (no check-in for more than
  65 minutes). A device whose IP or hostname changes stays the same row.
- **Login for changes** (the user's decision): uploading firmware, choosing devices to update, and
  deleting devices or releases need a login. *Designer's call*: one admin password, set in the container's
  environment (a gitignored compose override, like LibreNMS's), session cookie, logout.
- **Upload a release.** The admin uploads a `.upd` from `tools/fw_release.py`. The site reads its header
  (`parse_upd_header` layout: magic, version) and rejects other files. Several releases can be kept;
  the admin can delete one that no device is assigned to.
- **Choose devices** (the user's decision): the admin ticks devices and picks a release; those devices
  get it at their next check-in, the others keep what they run. *Designer's call*: an assignment belongs
  to a release, and a new upload starts with nothing ticked, so nothing updates unless the admin chooses
  it. Picking an older release than a device runs is allowed (a deliberate downgrade).
- **Device side of an update.** When the check-in reply names a release for this device, the device
  downloads that `.upd` over HTTP and feeds it to the existing receiver (`fwUpdateUploadBegin/Feed/Finish`
  in `src/fw_update_service.h`). *Designer's call*: it downloads the whole file and the receiver keeps
  only the blob for the free slot, as for a web upload, so no new file handling is needed. On a verified
  file it restarts into the new slot on trial, exactly as after a web upload. A refused file (bad
  signature, wrong key, wrong length) is reported at the next check-in and the device does not retry it.
- **Trial confirmation** (*designer's call*): today a trial firmware is kept after a valid SNMP request
  (right community) or authenticated web request. A device updated from the site may get neither (no
  LibreNMS polling it), so a check-in that the configured update server answers with a valid reply also
  counts as proof that the network works. The 10/60-minute rollback rules are unchanged.
- **Status follows the device.** The site shows each assigned device's progress from its check-ins:
  waiting (assigned, not yet checked in), downloading, on trial, done (runs the release, kept), rolled
  back, failed (with the device's reason). *Designer's call*: the assignment ends when the device reports
  the release kept, rolled back or refused, so a failing release is never retried in a loop. A download
  that broke off (network) is retried at the next check-ins, at most 3 times, then reported failed.
- **Shared key between site and devices** (the user's decision, 2026-11-06): the site and the
  firmware authenticate each other with a shared secret key, so a host on the LAN or a forged DHCP
  option 225 cannot make a device install anything, and a host without the key cannot appear in the
  device list. *Designer's calls* for the mechanism:
  - One key for the whole site (32 random bytes, shown as 64 hex characters). The site generates it
    (admin page, behind the login, with a "generate new key" action) and the admin enters it on each
    device from the serial menu or the web page settings. It is a new optional setting, never shown back
    in full (the menu and page say "set"/"not set"), cleared by a factory reset, and **never taken from
    DHCP**. A URL without a key means no check-ins (status "no key").
  - Each check-in carries a fresh random nonce from the device and a keyed BLAKE2b MAC (Monocypher
    `crypto_blake2b_keyed`; Python `hashlib.blake2b(key=...)`) over the request body. The site rejects a
    wrong MAC (the device shows "wrong key") and a nonce it has already seen from that serial number.
  - The reply carries a MAC over the device's nonce and the reply text. The device acts on a reply,
    including an update instruction and the trial confirmation, only if that MAC is right; otherwise
    it is treated as a failed check-in.
  - The download itself needs no MAC: the `.upd` is Ed25519-signed and its version must be the one the
    authenticated reply named.
  - The key and the URL must fit the settings file (`SETTINGS_JSON_MAX`, 1024 bytes, which older firmware
    also enforces) with every other field at its maximum; the builder sets the URL maximum accordingly.
- **Release identity** (*designer's call*): a release is identified by its version string. The site
  refuses a second upload with a version it already holds. The HIL test, whose `fwtest` variants reuse
  the version number, runs its own throwaway site instance.
- **Downgrades** (*designer's call*, safe because only the key holder can instruct a device): the admin
  may assign an older release, but the site refuses releases older than the first firmware with
  check-ins (it could never report back). An assignment with no report within 2 hours of the device
  being told is marked "lost" and ends.
- **Never in the way of the device's real job.** Check-ins and downloads are non-blocking on core 0:
  SNMP stays within 100 ms, the web page and serial menu stay responsive, mains detection on core 1 is
  untouched except for the flash-write pauses the update path already has. An unreachable or slow site
  costs a timeout, never a hang (watchdog). No buffer over ~256 bytes on the stack.

## Non-goals

- **No "update now"** (the user's decision): the site cannot reach a device; it only answers check-ins.
  An update starts within 30 minutes of being chosen, not at once.
- No HTTPS and no encryption: check-ins travel in clear text (hostname, addresses and versions are not
  secret); the shared key only authenticates them.
- No configuration of devices from the site (community, IP, ...). The site does not replace LibreNMS:
  no graphs, no alerts.
- No SNMP object for the URL or check-in state (not asked for; may come later).
- No scheduling (update windows) and no automatic rollout to all devices.

## Anti-goals

- A device that installs firmware the admin did not choose for it.
- A device that bricks, hangs or loses its network because the site is down, slow, returns garbage or
  a bad file; or that keeps downloading the same failing release.
- A device that misses mains changes or SNMP requests while it checks in or downloads.
- Passwords or the signing key on the site: the site never holds `secret/fw_signing_key`; releases are
  signed on the PC with `fw_release.py` as today.

## Constraints

- Project rules in `.clinerules` apply: logic in a pure library under `lib/` with native tests (HTTP
  request building, reply parsing, URL parsing, check-in timing and retry/state logic, DHCP text option),
  hardware/network in thin `src/` layers; parsers of network input tested with truncated and corrupted
  input and under `scripts/sanitize.sh`; every function documented; `-Wall -Wextra -Werror`.
- Settings: the new fields (URL, from-DHCP flag) are optional JSON fields with defaults, as
  `ipv6Address` was added (`readOptionalText`), so older files keep loading and `SETTINGS_VERSION`
  stays 1. A factory reset clears them.
- Networking through the Arduino-Pico lwIP W5500 stack (`WiFiClient` as `src/web_service.cpp` uses, or
  lwIP directly). Name resolution by lwIP DNS for a host name.
- Flash and RAM limits (70 % / 50 %); the firmware is about 390 KB today.
- Site in Docker on the user's PC next to LibreNMS (`librenms/test`), started with `docker compose up -d`,
  `restart: unless-stopped`, data (database and uploaded releases) in a named volume or bind mount that
  survives container rebuilds. *Designer's call*: port **8071** (LibreNMS is 8070). The user's firewall
  rules for 8070 do not cover it; the docs say what rule to add.
- Version bump (minor: new feature) in `include/version.h`, `CHANGELOG.md`, `TODO.md`, user docs
  (`docs/USER_MANUAL.md`, `docs/SETUP_MANUAL.md` for DHCP option 225 and the Docker site, `docs/TESTING.md`
  for new tests).

## Decided by the user

- The site runs in Docker, next to LibreNMS on this PC (it may run anywhere).
- Devices check in every 30 minutes with their IP address and hostname.
- The update URL is configurable in the firmware, and can also come from DHCP.
- The admin uploads new firmware to the site; devices download and install it.
- The admin selects which devices are updated.
- No "update now".
- Viewing the device list needs no login; uploading firmware (and choosing devices) needs one.
- The server and the firmware need a key to talk to each other.

## Left to the builder

- Site language and framework (*designer's suggestion*: Python with a small framework and SQLite, since
  the tools are Python and `tools/fw_release.py` already parses `.upd` headers), page design, where the
  site lives in the repo (*suggestion*: `update-server/` with its own compose file and tests).
- The exact check-in wire format (*suggestion*: `POST /api/checkin` with a small JSON or form body; a
  short plain reply the device can parse without a JSON library, e.g. `ok` or
  `update <version> <path>`, plus its MAC), and the download path. The exact MAC input (field order,
  separators) must be fixed in the pure library and the site's tests with shared test vectors.
- Exact retry/timeout values within the goals above, the menu option number and wording.
- Tests: native tests for the pure library; Python tests for the site; an opt-in HIL test
  (`RFPOWER_UPDATE_SERVER_TEST=1`) that runs the site locally, points a device at it, checks the
  check-in, assigns a release (the `fwtest` variants from `fw_release.py --test-variant` for refusal and
  rollback), and restores the device's URL setting and firmware afterwards.
