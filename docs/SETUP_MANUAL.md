# RF-Power-Monitor: setup manual

This manual takes a new RF-Power-Monitor from the box to a working mains alarm in LibreNMS. It is for the person who installs the device: you know your network (addresses, DHCP, switches), but you do not need to know the code. For daily use afterwards, see the [user manual](USER_MANUAL.md).

![System overview](diagrams/system-overview.svg)

## What you need

- The RF-Power-Monitor: a WIZnet **W5500-EVB-Pico-PoE** on the **RF-Power-Monitor carrier board** (rev 1.1: mains detector, temperature sensor, factory reset button). This manual is for that board. The [rev 2 board](../hardware/rf-power-monitor-rev2/README.md) is not built yet; it works the same way, but its part references differ (mains terminal J4, LEDs D3/D4, factory reset SW3).
- A **PoE switch port** (802.3af), or a USB power supply if you have no PoE.
- A **USB cable** (USB-C) and a PC with a terminal program such as PuTTY, for the first configuration.
- The **mains supply to watch**, wired to the carrier's screw terminal by a qualified electrician (see Safety).
- A **closed enclosure** for the board.
- **LibreNMS** (or another SNMP manager) on the network, if you want alerts.

## Safety first

The carrier board has a mains side: 230 V AC on the screw terminal J1, the fuse F1 and the optocoupler, inside the area marked "DANGER 230VAC" on the board.

- Mains wiring must be done by a **qualified person**, with the supply **switched off**.
- The board must sit in a **closed enclosure**; nobody may be able to touch the mains side.
- The supply to J1 must be **fused upstream**. F1 on the board (T 1 A) is for faults on the board, it does not replace the upstream fuse.
- Never work on the board while J1 is live. The low-voltage side (module, USB, Ethernet) is galvanically isolated from mains, but only while the board is undamaged and in its enclosure.

## 1. Assemble

1. **Mount the module** on the carrier, either soldered flat by its edges or plugged into two 1x20 female headers. The [carrier board README](../hardware/pico-poe-mains-carrier/README.md#mounting-the-module) shows both ways and which mounting holes to use.
2. **Mount the carrier** in the enclosure with M3 screws.
3. **Wire mains** to J1 (L and N; the wires enter from the same end as the USB port). The device only needs to see whether mains is there; it draws about 1 mA from it.
4. **Connect Ethernet** to the RJ45 on the module, from a PoE port. The board is powered over PoE. Without PoE, power it from the USB port.

Two LEDs on the carrier show mains directly, without the firmware: **green = mains on, red = mains off**. Check them once mains is connected.

## 2. Power on and connect to the serial menu

1. Connect the USB cable to the PC. The module appears as a COM port (Windows Device Manager → Ports).
2. Open PuTTY: **Serial**, that COM port, **115200** baud. (PuTTY sets DTR, which the device needs before it prints.)
3. Press **Enter**, or `?` and Enter. The menu appears:

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

4. Type `1` and Enter. The **status screen** shows everything the device knows. Look for:
   - `State: connected` and the `IP:` line: the address the device got from DHCP.
   - `Web login: user admin, default password "..."`: the password for the web page. **Write it down.**
   - `Mains power: present`.
   - `Switch: <name>, port <port>`: the switch port the device is plugged into, if the switch speaks LLDP.

The menu is available right after power-on, even before the network is up.

A new device comes with the module's own firmware or a development image. Install the release's `-usb.uf2` first (see [Installing or recovering over USB](#installing-or-recovering-over-usb)); later updates go over the network.

## 3. Network

The device uses **DHCP** by default and keeps retrying until it gets an address. If DHCP is what you want, you are done with this step. Tip: reserve the device's MAC address (status screen `MAC:`) in the DHCP server, so the address stays the same.

For a **static address**, use menu option `3`: it asks for the IP address, subnet mask and gateway, checks each one, and restarts the device after you answer `y`.

```
> 3
Press Enter to keep the value in brackets.
IP address [192.168.1.100]: 10.10.6.250
Subnet mask [255.255.255.0]: 255.255.0.0
Gateway [192.168.1.1]: 10.10.0.1

New mode: Static 10.10.6.250 / 255.255.0.0, gateway 10.10.0.1
Save and reboot? (y/n): y
```

IPv6 works by itself on a network with an IPv6 router (SLAAC, and DHCPv6 if the router asks for it). A fixed IPv6 address can be added with option `9`. See the README's [IPv6 section](../README.md#ipv6).

## 4. Name, SNMP and traps

Menu option `4` asks for these in turn. Press Enter to keep the value in brackets, `-` to clear it.

| Setting | What to enter |
|---|---|
| Community | The SNMP "password" your manager uses. Change it from `public`. |
| Hostname | The device's name, for example `power-serverroom`. It is the SNMP sysName, the name in DHCP, and the mDNS name (`power-serverroom.local`). Letters, digits and `-`. |
| sysContact, sysLocation | Who to call and where the box is. Shown in LibreNMS. Also on the web page (Device name). |
| Trap destination | The address of the LibreNMS server (it receives traps on UDP 162), or `-` for no traps. |
| Trap version | `2c` (recommended). |

```
> 4
Community [public]: s3cret
Hostname (sysName) [RF-Power-4EA22C]: power-serverroom
sysContact []: ops@example.com
sysLocation []: Server room, rack 4
Trap destination IP or dhcp []: 10.10.6.65
Trap version (1/2c) [2c]:
Save? (y/n): y
SNMP settings saved and active
```

These take effect at once; no restart.

## 5. Web page and password

Open **http://&lt;device address&gt;/** (or **http://&lt;hostname&gt;.local/**) in a browser and log in as `admin` with the default password from the status screen.

![The login page](images/web/web-login.png)

The page shows the status and lets you change the network, the name and the community, and restart the device. (In the pictures in this manual, the addresses, MAC and community are example values.)

![The status and settings page](images/web/web-status.png)

Set your **own web password** with menu option `6` (8 to 32 characters). It can only be set from the serial menu, so someone who learns it cannot lock you out. The page uses plain HTTP: use it on a trusted management network.

If you don't want the web page at all, turn it off with menu option `8`.

## 6. LibreNMS

1. **Install the device definition** on the LibreNMS server (once per server): copy the two definition files, the MIB and the logos as described in [docs/LIBRENMS.md](LIBRENMS.md#install).
2. **Add the device**: Devices → Add Device, the device's address or hostname, SNMP v2c, your community.

   ![LibreNMS: Add Device](images/librenms/add-device.png)

   LibreNMS detects it as **RF-Power-Monitor** and creates its sensors: *Mains power*, *Mains lost over 60 s*, *CPU* and *Ambient* temperature, and *Mains power changes since boot*; with firmware 3.1.0 and later also the two processor cores, memory, the settings flash as storage, the addresses, the serial number and the location you set. The device page then looks like this (contact replaced by a placeholder):

   ![LibreNMS: the device page with its sensors](images/librenms/device-overview.png)

3. **Traps**: set up snmptrapd on the LibreNMS server as described in the LibreNMS documentation; the device's traps then appear in its event log.
4. **Alert**: the standard rule **"State Sensor Critical"** alerts when *Mains lost over 60 s* goes critical, which happens when mains has been gone for 60 s. A shorter outage only shows as a warning and does not alert. Check under Alerts → Alert Rules that the rule exists and is on (ON); add a transport (e-mail, Teams, ...) to it if you want to be notified, not only see the alert in LibreNMS.

   ![LibreNMS: alert rules, with "State Sensor Critical"](images/librenms/alert-rules.png)

5. **Neighbours**: if the switch speaks LLDP, the device's Neighbours tab shows the switch and port, and with the RF-Power-Monitor plugin installed (docs/LIBRENMS.md, Install) the Overview page shows them in a **Switch** box. LibreNMS may try to add the switch as a device by itself; to stop that, see [docs/LIBRENMS.md](LIBRENMS.md#switch-and-port-lldp).

   ![LibreNMS: the switch and port on the Neighbours tab](images/librenms/device-neighbours.png)

## 7. Prove it works

Do this once, with the electrician, before you rely on the box:

1. With mains on, the status screen (and the web page) shows `Mains power: present` and `Mains alarm: none`. In LibreNMS, *Mains power* is OK.
2. **Switch mains off** for about **30 seconds**, then on again. The red LED lights, the status screen shows `Mains power: LOST`, then `present` again. LibreNMS logs the traps and shows a warning, but **no alert**.
3. **Switch mains off for 90 seconds.** After 60 s the status screen shows `Mains alarm: lost over 60 s`, the trap *mainsPowerLostTooLong* arrives, and LibreNMS opens the alert within seconds.
4. **Switch mains on.** The alarm clears and the alert recovers.

If something does not behave this way, see Troubleshooting in the [user manual](USER_MANUAL.md#troubleshooting).

## 8. Update site (optional)

The update site lists every device and updates the ones you choose (user manual, [From the update site](USER_MANUAL.md#from-the-update-site)). It runs in Docker, for example on the PC that runs LibreNMS. Set it up once:

1. Set the admin password and start it (from the repository):
   ```sh
   cd update-server
   # compose.override.yml (gitignored):
   #   services:
   #     rfupdate:
   #       environment:
   #         RFUPDATE_ADMIN_PASSWORD: "a long password"
   docker compose up -d --build
   ```
   It listens on TCP **8071** and keeps its data in the Docker volume `rfupdate-data`.
2. Let the devices reach it: on Windows, allow inbound TCP 8071 in the Windows firewall, and with WSL in mirrored mode also in the Hyper-V firewall (as for LibreNMS on 8070), as administrator:
   ```powershell
   New-NetFirewallRule -DisplayName "RF-Power update site 8071" -Direction Inbound -Protocol TCP -LocalPort 8071 -Action Allow
   New-NetFirewallHyperVRule -Name RFPOWER-Update-8071 -DisplayName "RF-Power update site 8071" -Direction Inbound -Protocol TCP -LocalPorts 8071 -Action Allow
   ```
3. Open `http://<pc>:8071/admin`, log in and press **Generate new key**. Copy the 64 hex characters: the site shows them only once.
4. On each device: serial menu option `10` (or the web page's **Firmware** page, card *Update server*), URL `http://<pc>:8071` and the key:
   ```
   > 10
   Update server: off (option 10)
   Press Enter to keep the value in brackets.
   Update server URL (http://host[:port][/path]), dhcp or none [none]: http://10.132.0.45:8071
   Shared key (64 hex characters from the update site), Enter keeps, clear removes [not set]: ****************************************************************
   Update server settings saved
   ```
5. Within a minute the device is on the site's Devices page. The status screen shows `Update server: ..., last check-in ... s ago: ok`. `badauth` means the key differs from the site's; a connection error means the device cannot reach the site (firewall, address).

**The URL from DHCP.** Instead of typing the URL on every device, enter `dhcp` in step 4 (the key is still entered on each device; it never comes from DHCP) and let the DHCP server send option **225** as text, for example:

- Kea (`kea-dhcp4.conf`):
  ```json
  "option-def": [ { "name": "rfpower-update-url", "code": 225, "type": "string" } ],
  "option-data": [ { "name": "rfpower-update-url", "data": "http://10.132.0.45:8071" } ]
  ```
- ISC dhcpd: `option rfpower-update-url code 225 = text;` and `option rfpower-update-url "http://10.132.0.45:8071";`
- dnsmasq: `dhcp-option=225,"http://10.132.0.45:8071"`

The device asks for option 225 when it is set to `dhcp` and takes it only from a DHCP answer it accepted for its own lease. A value longer than 127 characters or with other than printable ASCII is ignored.

## Firmware signing key

Devices accept a firmware update only when it is signed with **your** key (Ed25519). You create the key once, before the first field device is built. The person who builds releases needs a C++ compiler on the PC (g++ and gcc, as for the unit tests; the signing program is built from the same code the device verifies with) and Python 3.

1. **Generate the key, once:**

   ```powershell
   python tools/fw_keygen.py
   ```

   It writes the private key to `secret/fw_signing_key` (random, never printed, never overwritten: the tool refuses if the file exists) and the public key to `include/fw_public_key.h`, and prints the key's **fingerprint**, 16 hex digits. The status screen, the web page's Firmware page and `fw_update.py` show the same fingerprint, so you can see which key a device trusts.
2. **Commit `include/fw_public_key.h`.** It is public by design. The private key stays out of git (`secret/` is ignored).
3. **Back up `secret/fw_signing_key` now, outside this PC**: a password manager or an offline copy. Treat it like a root password.

What the key means:

- **Losing it**: no firmware the devices accept can be built again. Devices in the field keep running, but can only be changed over USB, with a new boot program and a new key. Back it up.
- **Leaking it**: anyone with the key and the device's web password (or access to the network path: the web page is plain HTTP) can install their own firmware. The devices cannot change their key over the network, so replacing a leaked key means a USB visit to every device.
- The web password alone installs nothing, and a file signed with another key is refused (`different key`) before it is written.

`tools/fw_keygen.py --force` replaces an existing key after you type `REPLACE`; it locks out every device built for the old key. Do not use it unless you mean to do USB visits.

### Field builds and development builds

- A normal build (`pio run -e pico`, `pio run -e pico -t upload`, the CI build and all tests) is a **development build**. It trusts the development key in `keys/dev`, whose private half is committed on purpose. **A device flashed with a development build accepts only development-signed files, and anybody who has this repository can sign for it.** Use development builds on the bench only.
- A **field build** trusts your key. Build the release files with:

  ```powershell
  python tools/fw_release.py --field
  ```

  It builds the firmware for both slots, signs both with `secret/fw_signing_key` (it stops if the key is missing, or does not match `include/fw_public_key.h`) and writes two files to `release/` (not in git):

  | File | Use |
  |---|---|
  | `rf-power-monitor-<version>.upd` | Network update: the web page's Firmware page and `tools/fw_update.py` |
  | `rf-power-monitor-<version>-usb.uf2` | USB install: boot program, the firmware in slot A, no firmware in slot B yet |

  Without `--field` the same command makes development-signed files.
- A field device never accepts a development-signed file, and a development device never accepts a field file. To move a device from one to the other, install the other kind's `-usb.uf2` over USB.

## Installing or recovering over USB

The first install of a device, a device with firmware older than 3.3.0 (its flash is laid out differently: the update needs the boot program), and a device that has no usable firmware slot (LED: 3 short blinks, 1 long, repeating) are installed over USB with the release's `-usb.uf2`:

1. Unplug USB, hold the **BOOTSEL** button on the module and plug USB in again. A drive called `RPI-RP2` appears.
2. Copy `rf-power-monitor-<version>-usb.uf2` to that drive. The device restarts after a few seconds. The settings in its LittleFS area are kept, since that area is not touched.
3. Check the serial status screen: `Firmware:` shows the version and slot A as *kept*, and `Firmware key:` the fingerprint from `fw_keygen.py` (a field build) or the development key's.

**Use only the release's `-usb.uf2`.** PlatformIO also writes a plain `firmware.uf2`; it holds only the firmware image for slot A, **without the boot program**, so a device flashed with it does not start (the way back is to copy a `-usb.uf2` the same way). With PlatformIO, `pio run -e pico -t upload` sends the right full image over the cable, but it is a development build: for field devices always use the release's `-usb.uf2`.

After that, updates go over the network: see "Updating the firmware" in the [user manual](USER_MANUAL.md#updating-the-firmware).

## Checklist

- [ ] Mains wired by a qualified person, enclosure closed, supply fused upstream
- [ ] Green/red LEDs follow mains
- [ ] Device has an address (DHCP reservation or static)
- [ ] Community changed from `public`, hostname, contact and location set
- [ ] Trap destination set to the LibreNMS server
- [ ] Own web password set (or web page turned off)
- [ ] Device in LibreNMS as RF-Power-Monitor with its sensors
- [ ] 30 s outage: warning, no alert; 90 s outage: alert, then recovery
- [ ] Firmware key generated once, `secret/fw_signing_key` backed up outside the PC, `include/fw_public_key.h` committed
- [ ] Field devices installed from the release's `-usb.uf2` (not `firmware.uf2`); the status screen's `Firmware key:` matches the key's fingerprint
- [ ] Optional: update site running with an admin password, devices set to its URL and key, each on its Devices page
