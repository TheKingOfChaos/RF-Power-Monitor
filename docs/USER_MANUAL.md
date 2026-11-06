# RF-Power-Monitor: user manual

The RF-Power-Monitor watches one mains supply and tells your monitoring system (LibreNMS or another SNMP manager) when it goes away. This manual is for day-to-day use: reading the device, reacting to an alarm, changing settings and solving problems. To install a new device, see the [setup manual](SETUP_MANUAL.md).

## How it works, in short

- The device checks the mains input every millisecond. A change is accepted after 5 ms, so short glitches are ignored.
- When mains goes away or comes back, it sends a **trap** (a message) to your monitoring system at once and counts the change.
- If mains stays away for **60 seconds**, it raises the **mains alarm** and sends one more trap, *mainsPowerLostTooLong*. In LibreNMS this opens the alert. A shorter outage is logged, but does not alert.
- When mains comes back, the alarm clears and the alert recovers.

![From the mains input to an alert](diagrams/mains-flow.svg)

## Where to look

### The LEDs

| LED | Meaning |
|---|---|
| Green LED on the carrier | Mains present (hardware, works even without the firmware) |
| Red LED on the carrier | Mains absent |
| Onboard LED on the module, off | Normal operation |
| Onboard LED blinking slowly (2 Hz) | The factory reset button is being held |
| Onboard LED blinking fast | Factory reset triggered, the device restarts |
| Onboard LED blinking steadily, device not answering | The internal flash could not be read at start-up; see Troubleshooting |
| Onboard LED: 3 short blinks, 1 long, repeating | The boot program found no firmware slot with a good signature; install a `-usb.uf2` over USB (see Updating the firmware) |

### The web page

Open **http://&lt;device address&gt;/** or **http://&lt;hostname&gt;.local/** and log in as `admin`. The page shows:

- **Mains power** (green or red badge) and how often it changed since the device started
- **Mains alarm**: *none*, or *lost over 60 s*
- Uptime, CPU and ambient temperature, the Ethernet link
- **Switch**: the switch and port the device is plugged into (if the switch speaks LLDP)
- Name, addresses, MAC address and network mode; the firmware version in the top bar

![The web page (addresses, MAC and community are example values)](images/web/web-status.png)

From the page you can change the network settings, the name and the SNMP community, update or switch the firmware (the **Firmware** page, see Updating the firmware), restart the device, and log out. A session ends after 15 minutes without use.

### The serial menu

Connect a USB cable and open a terminal (PuTTY: Serial, the device's COM port, 115200 baud), then press Enter. Option **1** shows the status screen with everything the device knows, including the default web password and the last restart cause. Option **5** shows the access log: rejected SNMP requests and failed web logins.

### LibreNMS

The device page shows the sensors *Mains power* (OK / warning), *Mains lost over 60 s* (OK / critical), *CPU* and *Ambient* temperature and *Mains power changes since boot*, the Ethernet port with its traffic, and in the **Switch** box the switch and port the device is plugged into (from LLDP, updated at each discovery; the box needs the RF-Power-Monitor plugin on the LibreNMS server). Since firmware 3.1.0 it also shows the device's location and contact (set them on the web page), the load of both processor cores and the memory in use (overview graphs *CPU* and *Memory*), the settings flash as storage, the IPv4 and IPv6 addresses on the port, the serial number, an **Inventory** entry, and IP/ICMP/TCP/UDP/SNMP traffic graphs under **Graphs → Netstats** (firmware 3.2.0, contact replaced by a placeholder):

![LibreNMS: the device page](images/librenms/device-overview.png)

Under **Logs → Event Log** you see what happened: the traps the device sent (mains lost and restored, restarts, wrong community), and changes LibreNMS noticed such as a new firmware version:

![LibreNMS: the device's event log](images/librenms/device-eventlog.png)

The **Neighbours** tab shows the switch and port the device is plugged into (from LLDP):

![LibreNMS: the Neighbours tab](images/librenms/device-neighbours.png)

The **Inventory** (top menu, or the device's Inventory tab) lists the device with its model and serial number:

![LibreNMS: the device in the inventory](images/librenms/device-inventory.png)

## When the alarm comes

*"Mains lost over 60 s"* in LibreNMS (or `Mains alarm: lost over 60 s` on the device) means the supply the device watches has been off for at least a minute.

1. **Check the supply itself**: the breaker, the fuse, the socket, upstream equipment. The red LED on the carrier confirms the device sees no mains.
2. **Check whether it is the device**: if the supply is fine but the red LED is lit, the device's mains connection (J1, or its fuse F1) may be the problem. That is work for a qualified electrician.
3. **When mains returns**, the alarm clears by itself and the alert recovers. Nothing needs resetting on the device.

The device keeps working without mains: it is powered over PoE (or USB), not from the mains it watches.

## Changing settings

Everything can be done in the serial menu; most things on the web page too.

| What | Serial menu | Web page | Takes effect |
|---|---|---|---|
| DHCP or static IPv4 | 2 / 3 | Network | after a restart (automatic) |
| Static IPv6 address | 9 | IPv6 | after a restart (automatic) |
| Name (hostname / sysName), contact, location | 4 | Device name | at once |
| SNMP community | 4 | SNMP community | at once |
| Trap destination and version | 4 | | at once |
| Web password | 6 (`-` = back to the default) | | at once, logs browsers out |
| New random default web password | 7 | | at once |
| Web page on/off | 8 | Turn off… (only the menu turns it on again) | at once |

The device checks every value before it uses it and tells you what is wrong. Nothing changes until you confirm. Settings are kept across restarts, power cuts and firmware updates: they are stored twice, so a power cut while saving cannot lose them.

## The serial menu

The serial menu can do everything the device offers. It works even when the network does not, so it is the way in when nothing else is.

### Connecting

1. Connect a USB cable from the module (USB-C) to a PC. It appears as a COM port (Windows Device Manager → Ports).
2. Open PuTTY: **Serial**, that COM port, speed **115200**. Other terminals work too, as long as they set DTR (PuTTY does).
3. Press **Enter**. The menu appears; `?` and Enter shows it again at any time.

```
=== RF-Power-Monitor Menu ===
1. Show status
2. Use DHCP
3. Set static IP
4. SNMP settings
5. Access log
6. Web password
7. New default web password
8. Web interface on/off
9. Static IPv6
10. Update server
?. Show this menu
>
```

Type the number and Enter. Questions show the current value in brackets: **Enter** keeps it, **`-`** clears it (where clearing makes sense). Every value is checked; a wrong one is explained and asked for again. Nothing is changed until you answer **`y`** to the final question; **`n`** cancels and changes nothing. A line longer than the device accepts is ignored (`Input too long`).

Only one program can use the COM port at a time: close PuTTY before you update the firmware. The device also prints messages by itself, for example `Mains power LOST at ...` or `Network up after ... ms`; they can appear in the middle of the menu and do no harm.

### 1. Show status

Shows everything the device knows. An example (with example addresses):

```
Mains power: present (22 changes, last change 1053 s ago)
Mains alarm: none
SNMP: 802 packets, 802 responses (get 437, getnext 0, set 0), bad community 0, bad version 0, malformed 0
SNMP v2c: 792 requests, 365 getbulk
CPU temperature: 40.4 C
Ambient temperature: 27.1 C
Ethernet: link up, 100 Mbit/s, in 184054263 bytes / 1351415 packets, out 149671 bytes / 1002 packets
Ethernet drops: 0 discarded (no buffer), 0 errors, 211829 unknown protocol, 0 send errors
SNMP security: 0 rate limited, 0 access log events
SNMP community: "public"
sysName: "RF-Power-4EA22C"
sysContact: ""
sysLocation: ""
Trap destination: "192.0.2.65" (4 sent, 0 failed)
Trap destination source: fixed (menu option 4)
Trap version: v2c
Hostname: RF-Power-4EA22C (DHCP, mDNS RF-Power-4EA22C.local)
Switch: core-sw1, port ge-0/0/9.0
Web interface: on (TCP port 80)
Web login: user admin, default password "k7wqz3mtd9" (change with option 6)
Core 0 stack: 6272 bytes never used
CPU load: core 0 0 %, core 1 1 % (last minute)
RAM used: 133444 of 262144 bytes
LittleFS used: 32768 of 262144 bytes
Serial number: E6641CB2CF4CA22C
Last restart: reboot (0 watchdog restarts since power-on)
Mode: DHCP
Stored static IP: 192.168.1.100 / 255.255.255.0, gateway 192.168.1.1
Stored static IPv6: none (option 9)
DHCPv6: off (no router asked for it)
Live interface:
State: connected
MAC: 02:00:00:12:34:56
IP: 192.0.2.53
Subnet: 255.255.255.0
Gateway: 192.0.2.1
IPv6: FE80::FF:FE12:3456
IPv6: 2001:DB8:0:1:0:FF:FE12:3456
DNS: 192.0.2.10
```

| Line | Meaning |
|---|---|
| Mains power | `present` or `LOST`, how many changes since start-up and how long ago the last one was. `unknown (monitor not running)` means the mains monitor on core 1 is not running: restart the device, and report it if it stays. |
| Mains alarm | `none`, or `lost over 60 s` while mains has been gone for 60 s or more. |
| SNMP, SNMP v2c | Requests the device answered, and those it dropped (wrong community, wrong SNMP version, broken packets). |
| CPU / Ambient temperature | The chip temperature, and the room sensor on the carrier (`no sensor` without the carrier). |
| Ethernet | Link and speed, and traffic counters since start-up. |
| Ethernet drops | Frames the device could not take or does not use. `unknown protocol` grows on a normal network (for example spanning tree) and is no fault. |
| SNMP security | Requests rejected by the rate limit, and entries in the access log (option 5). |
| SNMP community … Trap version | The SNMP settings (option 4); `Trap destination` also counts sent and failed traps. `Trap destination source` says whether the destination is fixed or comes from DHCP. |
| Hostname | The device's name, as used for DHCP and mDNS. |
| Switch | The switch and port the device is plugged into (LLDP), or `none seen`. |
| Web interface, Web login | Whether the web page is on, and its login: the **default password** is shown here until you set your own. |
| Core 0 stack | A health figure for the firmware; it should stay well above 0. |
| Last restart | Why the device last started: `power-on`, `reset pin`, `reboot` or `watchdog` (the firmware hung and was restarted), and how many watchdog restarts happened since power was applied. |
| Mode, Stored static IP, Stored static IPv6 | The configured network mode and the stored static settings (used only in static mode, or when set). |
| DHCPv6 | `off` unless the IPv6 router asks for DHCPv6; then the leased address. |
| State | `connected`; `link up, waiting for DHCP lease` (keeps trying); `no Ethernet link, check cable`; or `W5500 not detected, check wiring`. |
| MAC … DNS | The addresses in use right now. IPv6 addresses are marked `(static)` or `(dhcpv6)` when they come from those. |

### 2. Use DHCP

Switches the network to DHCP (the default). Confirm with `y`; the device saves and restarts. The stored static address is kept for later.

### 3. Set static IP

Asks for the IP address, subnet mask and gateway in turn, and checks each: four numbers 0-255; a contiguous mask (`255.255.252.0` is fine, `255.0.255.0` is not); not the network or broadcast address; the gateway in the same subnet. Confirm with `y`; the device saves and restarts with the new address.

### 4. SNMP settings

Asks in turn:

| Question | Accepted | Note |
|---|---|---|
| Community | 1-31 printable characters | The SNMP "password". After saving, the old one gets no answer. |
| Hostname (sysName) | 1-32 letters, digits and `-`, not starting or ending with `-` | Also the DHCP name and `<name>.local`. |
| sysContact, sysLocation | 0-63 printable characters | `-` clears. |
| Trap destination | an IPv4 or IPv6 address, `dhcp`, or `-` for no traps | `dhcp` takes the destination from the DHCP server (option 224). An IPv6 destination must be a global address, not `fe80::`. |
| Trap version | `1` or `2c` | `2c` is recommended; an IPv6 destination needs `2c`. |

Confirm with `y`. The new settings work at once; no restart.

### 5. Access log

The latest rejected requests and failed web logins, newest first. A repeat of the newest entry (same source, same reason) is counted on that line instead of adding a new one. Each new entry is also printed when it happens.

```
Access log: 3 entries, 128 events since boot
  192.0.2.65      rate limited   x104    last 2 s ago
  192.0.2.65      bad version    x1      last 40 s ago
  192.0.2.80      bad community  x23     last 61 s ago
```

| Entry | Meaning |
|---|---|
| `bad community` | An SNMP request with the wrong community: a manager with an old setting, or someone guessing. |
| `bad version` | SNMPv3 or another version the device does not speak. |
| `malformed` | A broken or oversized SNMP packet. |
| `rate limited` | Too many requests from one source (more than 50 per second). |
| `set rejected` | Someone tried to change a value over SNMP; the device is read-only. |
| `web login failed`, `web locked out` | A wrong web password; five in a row lock the login for 30 s. |

### 6. Web password

Sets your own password for the web page: 8-32 characters, no spaces, typed twice (shown as `*`). `-` goes back to the default password. Every browser is logged out. The password can only be set here, not on the web page.

### 7. New default web password

Replaces the default web password with a new random one after `y`, and shows it. Use it if the default password became known; the old one stops working at once. An own password from option 6 stays in use.

### 8. Web interface on/off

Turns the web page off or on after `y`. Off closes the web port and logs every browser out; SNMP and the serial menu keep working. The setting is kept across restarts.

### 9. Static IPv6

Asks for a static IPv6 address and an optional IPv6 gateway. The address is used as a /64 next to the automatic ones; it must be a global (`2001:...`) or local (`fd..`) address, not `fe80::`. Leave the gateway empty to use the routers that announce themselves. `-` removes a value. Confirm with `y`; the device saves and restarts.

If a wrong network setting makes the device unreachable, use option 2 (DHCP) or 1 to see what it uses, or do a factory reset.

### 10. Update server

Connects the device to the update site (see [Updating the firmware](#from-the-update-site)). It asks for two things; Enter keeps the value in brackets:

| Prompt | What to enter |
|---|---|
| Update server URL | `http://<pc>:8071` (the site's address), `dhcp` to take it from DHCP option 225, or `none` for no check-ins. Only `http://host[:port][/path]`, at most 127 characters. |
| Shared key | The 64 hex characters from the site's admin page. Typed as `*` and never shown again (status: `key set`). `clear` removes it. |

The settings are saved at once and the first check-in follows within a minute. The status screen shows them on one line, e.g. `Update server: http://10.132.0.45:8071 (fixed), key set, last check-in 4 min ago: ok`, or `Update server: off (option 10)`. The same settings are on the web page's **Firmware** page (card *Update server*). A factory reset removes the URL and the key.

## Factory reset

Use this if you can no longer reach the device (wrong static address, unknown community) and have no USB access, or to hand the device on.

1. Press and hold the button **SW1** on the carrier (under the module). The onboard LED blinks slowly.
2. Keep holding for **10 seconds**, until the LED blinks fast. Then let go.
3. The device restarts with the defaults: DHCP, community `public`, name `RF-Power-<last 6 MAC hex digits>`, no contact, location or trap destination, no static IPv6, web page on and back on its default password.

Letting go before 10 s cancels the reset. The device keeps working while you hold the button. The default web password itself is not changed by a reset; it is still on the status screen. After a reset, set the community, name and trap destination again (setup manual, step 4).

## Updating the firmware

From firmware 3.3.0 the device updates itself over the network, from its web page or from a script. Settings are kept. Only firmware **signed with your firmware key** is accepted (see the [setup manual](SETUP_MANUAL.md#firmware-signing-key)), so the web password alone cannot install anything. If a new firmware does not work, the device goes back to the old one by itself.

### How it works

The flash holds two firmware **slots**, A and B, and a small boot program that checks the signature of a slot before it starts it. An update is always written to the slot that is **not** running; the running firmware is never touched. After the upload the device restarts into the new slot **on trial**:

| State | Meaning |
|---|---|
| **Kept** | The running firmware is confirmed. The other slot holds the previous version as a fallback. |
| **On trial** | The firmware is new and not yet confirmed. It becomes *kept* as soon as it has answered one valid request: an SNMP request with the right community (LibreNMS polling does this within seconds) or an authenticated web request (logging in to the page). |
| **Rolled back** | The trial failed and the device is back on the previous firmware: it crashed or hung (the watchdog restarted it three times in a row), or the network was up for 10 minutes without a valid request, or 60 minutes passed on trial whatever the network did. The Firmware page, the status screen and SNMP say why (`rolled back (crash)` or `rolled back (unreachable)`), and the device sends the *firmwareRolledBack* trap. The release that failed is then **erased from the other slot** (*Other slot* reads `empty`), so the device can never go back to it later. |

Only a restart by the watchdog counts as a crash. Power cuts, PoE restarts and planned restarts (menu, web page, factory reset, the restart after an update) never do. The 10 minutes count only while the Ethernet link is up and the device has an address, so a switch that is down does not throw away a good release.

If a **kept** firmware later crashes three times in a row (a bug that only shows after days), the boot program starts the other slot, if its signature is good, and the device reports a **fallback**: *Last update* reads `fallback (crash)` and the trap *firmwareRolledBack* is sent. The same text is used when the boot program finds that the active slot fails its signature check at start-up (damaged flash): that case is recorded with the crash result. A fallback only ever reaches the previous kept release: a release that was rolled back has been erased, so after a rollback the other slot is `empty` and a fallback is not possible until the next update is kept. The crashing firmware then keeps restarting, as it would without a second slot.

Downgrades are allowed: any file signed with your key installs, an older version too.

### From the web page

1. Open the device's web page and click **Firmware** in the top bar. It shows the running slot and version, whether it is *kept* or *on trial*, the version in the other slot, the last update result and the **key id** the device trusts.
2. Under **Upload an update**, choose the `rf-power-monitor-<version>.upd` file (about 0.8 MB) and press **Upload**. The page shows the progress. The device checks the signature while it receives the file, then answers `verified` and restarts. The page reloads after 20 seconds.
3. The page shows **on trial** and then **kept** once the device has been reached (a few seconds with LibreNMS polling it, or when you log in again). Allow about 30 seconds from the upload to *kept*.
4. **Switch firmware** restarts the device into the other slot, on trial, if that slot holds a good firmware and the running one is *kept*. Use it as a manual way back when a release works but you do not want it.

Things to know while updating:

- **The device is busy during an upload.** Each flash write takes tens of milliseconds and pauses the whole device, so SNMP answers can be slow or missed and the web page is slow. **Mains detection can be late by up to one flash erase (tens of milliseconds) while an upload is running.** The upload itself takes under half a minute: do not update during a mains test where the exact time matters.
- **Uploads are refused while the firmware is on trial** (`on trial`), while another upload is running, and while a restart is pending. Wait until the page says *kept*, or until the trial has rolled back.
- **A rejected upload empties the other slot** until the next good upload: the device starts writing the other slot as soon as the file's header checks out, so a file that then fails (bad signature, truncated, connection lost) leaves that slot unusable. The running firmware is not affected. The page then reads *Other slot: not usable* and *Last update: rejected: ...*, for example `bad signature`, `truncated`, `different key` (signed with a key this device does not trust, HTTP 403) or `on trial`. Upload again with a good file.
- Update **one device at a time** if you do it by hand: a device on trial that nobody asks for 10 minutes rolls back.

### From the update site

From firmware 3.4.0 a device can check in with the **update site**, a small website in Docker (setup manual, [step 8](SETUP_MANUAL.md#8-update-site-optional)). Every 30 minutes it reports its serial number, hostname, IPv4 and IPv6 addresses and firmware. The site's **Devices** page lists them (no login needed) and shows a device as *offline* after 65 minutes without a check-in.

To update devices from the site:

1. Log in to the site's **Admin** page and upload the release's `rf-power-monitor-<version>.upd`. The site refuses files older than 3.4.0 (they could never report back), a version it already has, and a file whose signed firmware is another version than its header says.
2. Tick the devices, pick the release and press **Update ticked devices**.
3. At its next check-in (within 30 minutes) each ticked device downloads the file and installs it exactly as an upload from the web page: the signature is checked, the other slot is written, the device restarts on trial and rolls back if the new firmware does not work. A check-in that the site answers confirms the trial, so no LibreNMS polling or login is needed.
4. The Devices page follows each update: *waiting*, *told*, *on trial*, then **done**, **rolled back**, **refused** (bad signature, another signing key, on trial...), **failed** (three broken downloads) or **lost** (no report within 2 hours). A failed or refused release is not tried again until you assign it again.

Only the site holding the **shared key** can tell a device to update: every check-in and every answer carries a keyed BLAKE2b code, and a device ignores an answer without the right one. A device without the key does not check in at all. Even with the key, a device installs only firmware signed with your firmware key. Check-ins are not encrypted: the hostname, addresses and versions they carry are readable on the network. There is no "update now": the site can only answer check-ins.

While a device downloads from the site it serves SNMP and the web page as usual (the download is slower than the network allows, on purpose, so SNMP requests are never lost). The flash writes pause the device briefly, as for a web upload.

### With a script: `tools/fw_update.py`

For many devices, or to avoid the browser (Python 3, standard library only; it needs `tools/fw_release.py` beside it):

```powershell
$env:RFPOWER_WEB_PASSWORD = "<the web password>"   # or leave it out and the script asks
python tools/fw_update.py release/rf-power-monitor-3.3.0.upd 10.0.0.21 10.0.0.22
python tools/fw_update.py file.upd --hosts-file devices.txt --continue --timeout-min 15
```

For each device, one at a time, it logs in as `admin`, compares the file's key id with the device's (a device that trusts another key is refused before anything is uploaded), uploads, waits for the restart and polls until the new version runs **in the other slot** and is **kept** (so a rebuilt file of the version already running is not reported before the device has restarted into it). It prints one line per device: `updated`, `rejected: <reason>`, `rolled back: <last result>`, `not installed: ...` (the device restarted on its old firmware without a rollback), `unreachable` or `timeout`. `rejected: no reply (<reason>)` means the device refused the file but its answer was lost on the way. It **stops at the first device that is not updated**, so a bad release does not reach the whole fleet; `--continue` carries on. The exit code is 0 only when every device was updated. The password is never an argument and never printed.

### Over USB (first install, or a device that cannot start)

The network update replaces only the two firmware slots. The boot program and the flash layout change only over USB. A device with firmware older than 3.3.0 needs this once, and so does a device whose slots both fail their check (the onboard LED then blinks 3 short and 1 long, forever).

1. Get the release's `rf-power-monitor-<version>-usb.uf2` (see the [setup manual](SETUP_MANUAL.md#installing-or-recovering-over-usb)).
2. Unplug USB, hold the **BOOTSEL** button on the module and plug USB in again. A drive called `RPI-RP2` appears.
3. Copy the `-usb.uf2` file to that drive. The device restarts with the new firmware in slot A and nothing in slot B. Settings are kept.

**Use only the release's `-usb.uf2` (or a build's `firmware_full.uf2`) on the BOOTSEL drive.** The plain `firmware.uf2` that PlatformIO also writes holds only the firmware image, without the boot program: copied to the drive it leaves a device that does not start, and the way back is to copy a `-usb.uf2` file the same way. With PlatformIO installed, `pio run -e pico -t upload` sends the right file over the USB cable (close PuTTY first).

The boot program verifies the active slot at every start, which adds about a second to the start-up.

## Troubleshooting

| Problem | What to do |
|---|---|
| No alert although mains is off | Is the device reachable (LibreNMS shows it up)? The alert needs 60 s without mains. Check the trap destination (status screen `Trap destination: ... (N sent, ...)`) and the firewall on the LibreNMS server (UDP 162). Without traps, LibreNMS notices at its next poll (every 5 minutes). |
| Device has no address | Status screen `State:` says why: `no Ethernet link, check cable`, or `link up, waiting for DHCP lease` (it keeps retrying; check the DHCP server or the switch port). |
| Unreachable after setting a static address | Serial menu: option 1 shows the address in use; option 2 goes back to DHCP. Or factory reset. |
| LibreNMS gets no answers | The community must match (status screen `SNMP community`). Polling very hard is rate limited: the access log (option 5) shows `rate limited` or `bad community`. |
| Web page does not load | Use `http://`, not https. Status screen `Web interface:` must say `on`; option 8 turns it on. |
| Forgot the web password | Serial menu option 6, enter `-`: the default password from the status screen works again. |
| "Too many failed logins" | Five wrong passwords lock the login for 30 s. Wait, then log in with the right password. |
| "Form expired" or "Not logged in" | The session ended (15 minutes idle, restart, password change, or a third browser). Reload and log in. |
| Someone else knows the default web password | Serial menu option 7 makes a new random one; or set your own with option 6. |
| The device restarted by itself | Status screen `Last restart:` says why. `watchdog` means the firmware hung and the 8 s watchdog restarted it; settings are kept. If it repeats, report it. |
| Onboard LED blinks steadily, no network, no menu | The internal flash could not be read at start-up. Unplug and plug in again; if it stays, reinstall the firmware (see Updating). |
| LED blinks 3 short, 1 long, no network, no menu | Neither firmware slot has a good signature. Install a `-usb.uf2` over USB (see Updating). |
| Upload says `on trial`, `different key`, `bad signature` or `truncated` | `on trial`: wait until the Firmware page says *kept*. `different key`: the file is signed with a key this device does not trust (compare the key ids). `bad signature`, `truncated`: the file is damaged, get it again. |
| `Last update:` says `rolled back` | The new firmware crashed or could not be reached, and the device went back by itself. The bad version is erased from the other slot (`Other slot: ..., empty`), so *Switch firmware* is not possible and a fallback cannot reach it. Upload a fixed release. |
| The BOOTSEL drive took `firmware.uf2` and the device does not start | That file has no boot program. Copy a `-usb.uf2` file to the drive the same way. |
| No text in PuTTY | PuTTY must be set to Serial, the right COM port, 115200. Press Enter. Only one program can use the COM port at a time. |
| Temperature seems high | The ambient sensor sits on the board and warms with it; a few degrees above the room is normal. |

More detail: the [README](../README.md) (every menu option, the web page, network and IPv6), [docs/MIB.md](MIB.md) (all SNMP objects and traps) and [docs/LIBRENMS.md](LIBRENMS.md).
