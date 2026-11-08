# Security policy

## Supported versions

Security fixes go into the latest release only. Update to it before reporting, if you can.

| Version | Supported |
|---|---|
| 3.6.x (latest) | Yes |
| Older | No: update to the latest release ([docs/USER_MANUAL.md](docs/USER_MANUAL.md#updating-the-firmware)) |

## Reporting a vulnerability

Please **do not open a public issue** for a security problem.

Report it privately on GitHub: open the repository's **Security** tab and choose **Report a vulnerability** ([direct link](https://github.com/TheKingOfChaos/RF-Power-Monitor/security/advisories/new)). Only the maintainers see the report.

Useful to include:

- The firmware version (status screen, web page, or `sysDescr`) and whether it is a release file or your own build
- What an attacker needs (network access to the device, a login, physical access) and what they gain
- Steps or a script to reproduce it, or the packet or request that triggers it

What happens next:

- You get a first answer **within 7 days**.
- Once the problem is confirmed, we agree with you on a fix and a date to publish it. The fix comes as a new release, with the problem described in its notes and in [CHANGELOG.md](CHANGELOG.md).
- You are credited in the advisory unless you prefer not to be.

## What is in scope

- The firmware: the SNMP agent, the web page and its login, the firmware update (upload, signature check, slot switch, rollback), the update-site client, DHCP, DHCPv6, LLDP, NTP and syslog handling, and the settings storage
- The boot program (`boot/`), which checks a firmware's Ed25519 signature before starting it
- The update site (`update-server/`)
- The tools in `tools/` that sign and send firmware and set up new boards

## Known limits, by design

These are not vulnerabilities, but reports that show they can be abused in a worse way than described here are welcome:

- **SNMP v1 and v2c** send the community in clear text, as those protocols do. The agent is read-only: every SetRequest is refused. Keep SNMP on a management network.
- **The web page is plain HTTP**: no TLS. The login is protected by a salted password hash on the device and a lockout after five wrong attempts, but someone who can read the traffic can read the session. Use it on a trusted network, or turn it off (serial menu option 8).
- **The update site talks plain HTTP.** Every check-in and answer carries a keyed BLAKE2b code, and every firmware image is signed with Ed25519 and checked by the device, so a third party can neither tell a device to update nor give it firmware it would start. They can see that a device checks in.
- **Physical access is full access.** The USB serial menu has no password (it can change every setting, including the web password), and the BOOTSEL button lets anyone flash the board.
- **The development signing key** in `keys/dev/` is published on purpose. A board flashed with a development build (`pio run`, CI) accepts firmware signed by anyone. Field devices use a field build, which trusts only the field key (key id `5768aa99900e4ad9` for the published releases). Never use a development build on a device in service.

## Security features

For reference when judging a report: signed firmware with automatic rollback ([docs/USER_MANUAL.md](docs/USER_MANUAL.md#updating-the-firmware)), SNMP rate limiting and an access log, web login lockout, settings validated on load with two copies kept, a hardware watchdog, and protocol parsers tested with truncated and corrupted input under AddressSanitizer and UndefinedBehaviorSanitizer ([docs/TESTING.md](docs/TESTING.md)). The code is scanned by CodeQL on every push.
