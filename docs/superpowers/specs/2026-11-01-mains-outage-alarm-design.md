# Mains outage alarm (lost for more than 60 s): design

Replaces TODO 2.5 "Optional: LibreNMS trap handler for mainsPowerLost/Restored (PHP, would have to go upstream)".

## Intent

The user wants LibreNMS to raise an alert, inside LibreNMS (alert list and alert log, no e-mail or other
transport), only when mains has been gone for more than 60 s. Short outages must not raise an alert.

Today the "State Sensor Critical" rule opens the moment mains is lost, because the `mainsPowerStatus`
state sensor maps "lost" to critical and the trap script polls on every mains trap. A rule delay does not
help: LibreNMS creates the alert and its alert-log entry at the poll and only holds back notifications for
the delay (checked in `LibreNMS/Alert/RunAlerts.php` of the running container). LibreNMS also only checks
rules when it polls, and the device does not report how long mains has been gone.

So the device itself decides: once mains has been lost for 60 s it raises a new alarm object and sends a
trap. LibreNMS gets a state sensor on that object which is critical when raised, and the existing rule does
the rest.

## Goals

- New MIB object `mainsPowerAlarm` (`1.3.6.1.4.1.63050.6.0`, INTEGER): `0` = none, `1` = mains lost for
  60 s or more. Answered in v1 and v2c, walked after `ambientTemperature`, in `mibs/RF-UPS-MONITOR-MIB.txt`
  with an enumeration (`none(0)`, `lostTooLong(1)`), validated with `snmptranslate`.
- New trap `mainsPowerLostTooLong` sent once when the alarm is raised (*designer's call* on the name: an SMI
  descriptor must be unique, so the trap cannot share the object's name; the `mainsPower` prefix keeps the
  trap script's name match working): v2c notification
  `1.3.6.1.4.1.63050.0.3`, v1 enterprise-specific trap 3 under enterprise `1.3.6.1.4.1.63050`. Varbinds:
  `mainsPowerStatus`, `mainsPowerChanges`, `mainsPowerLastChange`, `mainsPowerAlarm` (v2c also
  `snmpTrapAddress.0`, like the other mains traps). Uses the configured trap version and destination.
- The alarm clears to `0` when mains is restored. No extra trap for that: `mainsPowerRestored` is already
  sent and makes LibreNMS poll.
- LibreNMS definition (`librenms/os_discovery/rfupsmonitor.yaml`): a state sensor "Mains lost over 60 s" on
  `mainsPowerAlarm`, `none` generic 0, `lostTooLong` generic 2. The `mainsPowerStatus` sensor's `lost`
  changes from generic 2 to generic 1 (warning), so a short outage shows yellow but does not match
  "State Sensor Critical".
- The trap script `librenms/test/snmptrap-agent-addr.php` also polls on the new trap (`63050.0.3`), so the
  alert opens a few seconds after the 60 s mark. Without that script (stock LibreNMS) the alert comes at
  the next 5-minute poll after 60 s.
- Verified on the user's LibreNMS with real mains cuts via the Dirigera outlet: about 30 s gives no alert
  (sensor shows warning), about 90 s opens the alert at about 60 s and recovers when mains returns.

## Behaviour details

- Timing counts from the debounced change time core 1 records (`MainsSnapshot.lastChangeMs`), not from
  when core 0 noticed. Raised when mains is lost and `now - lastChangeMs >= 60000` ms.
- The device booted without mains (no change since boot, `present == false`): counts from boot, so the alarm
  is raised 60 s after start-up.
- If no trap can be sent when the alarm is raised (no IP or link yet, no destination configured, DHCP option
  224 destination not received yet), one pending alarm trap is held and sent once at the first moment a
  destination exists; it is dropped if mains is restored first. One send attempt, as for every trap (a send
  error counts in `trapsFailed`). *Designer's call.*
- Once raised it stays raised until mains is present again, also past the 49.7-day `millis()` wrap. A new
  loss after a restore starts a new 60 s.
- Restored and lost again within 60 s (flapping): the 60 s restarts at each loss, so no alarm. (Intended:
  the user cares about one continuous outage.)
- The 60 s is fixed (`MAINS_ALARM_AFTER_MS = 60000`), not a setting. *Designer's call*: the user asked for
  60 s; making it settable adds a settings field, migration and menu item. Easy to add later.
- `mainsPowerStatus`, its traps and their meaning do not change (backward compatible); v1 behaviour of
  existing objects unchanged.
- Status screen: one line or field showing the alarm (e.g. `Mains alarm: lost over 60 s` / `none`).
  *Designer's call*; the web page gets the same as a row.

## Structure

- Pure logic in `lib/power_monitor` (`mains_alarm.h/.cpp`): a small class fed the mains snapshot
  (present, changes, lastChangeMs) and `nowMs`, returning the alarm state and whether it was just raised
  (edge for the trap). Host tested.
- `src/main.cpp` core 0 loop: next to the existing mains-change report, update the alarm and call a new
  `snmpNotifyMainsAlarm()` on the raise edge. `captureState()` fills a new `SnmpDeviceState.mainsAlarm`.
- `lib/snmp_agent`: MIB entry and trap builders (v1 and v2c), alongside `snmpBuildMainsTrap*`.

## Tests

- Native (`test/native`): alarm logic (59 999 ms no, 60 000 ms yes; restore clears; new loss restarts;
  boot without mains; stays raised across the `millis()` wrap; raise edge reported once); MIB get/next for
  `.6.0`; trap encoding byte-exact against net-snmp `snmptrap` captures (v1 and v2c).
- `scripts/host_agent_check.sh` walks the new object with net-snmp.
- HIL (`test/hil`, Dirigera mains switch, already opt-in via `RFUPS_MAINS_SWITCH`): a ~30 s cut keeps
  `mainsPowerAlarm` at 0; a ~90 s cut reads 1 after 60 s and 0 after restore. The trap is checked where the
  existing trap HIL tests can receive on UDP 162 (they skip while LibreNMS's snmptrapd holds the port).
- LibreNMS: manual real-mains verification above, recorded in `docs/LIBRENMS.md`. *Designer's call*: no
  automated LibreNMS API test (it would need an API token in the user's LibreNMS).

## Docs

`docs/MIB.md` (object, trap, alarm timing), `docs/LIBRENMS.md` (new sensor, why not a rule delay, warning
vs critical, rediscovery needed after updating the definition), `CHANGELOG.md` (Added, Unreleased),
`TODO.md` (replace the PHP item and tick it), README if it lists MIB objects or traps.

## Non-goals

- No LibreNMS PHP trap handler upstream, no notification transports.
- No configurable delay, no alarm for flapping power.
- No change to the firmware's mains detection or the existing traps.

## Left to the builder

Names of functions and the status line wording; where in the MIB table code the entry goes; how the HIL
test waits (polling `mainsPowerAlarm` over SNMP is fine).
