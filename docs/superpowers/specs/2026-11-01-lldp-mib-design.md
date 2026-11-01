# LLDP-MIB: the switch and port on the device's LibreNMS page

Date: 2026-11-01. Status: draft.

## Intent

Since 2.6.0 the device reads its switch's LLDP and shows the switch and port on
the status screen and web page. The user wants the same on the device's page in
LibreNMS ("Neighbours"): RF-UPS-Monitor is on switch `r169`, port `ge-0/0/9.0`,
without adding the switch (a Juniper EX2200-C) to LibreNMS.

LibreNMS learns neighbours by asking each device over SNMP for its LLDP-MIB
(IEEE 802.1AB-2005 Annex F, `1.0.8802.1.1.2`): every OS in LibreNMS uses
`LibreNMS/OS/Traits/LldpMib.php`, which walks `lldpRemTable` and
`lldpLocPortTable` (and LLDP-V2-MIB only if `lldpRemTable` is empty). So the
device answers those tables from the neighbour it already keeps. No LibreNMS
change is needed.

Success: after a LibreNMS discovery of the device, its Neighbours page lists
`r169`, remote port `ge-0/0/9.0`, on local port `eth0` / "W5500 10/100 Ethernet".

## Goals

1. **lldpLocPortTable** (`1.0.8802.1.1.2.1.3.7.1`), one row, lldpLocPortNum 1
   (the device's one port, = ifIndex 1):
   - `.2.1` lldpLocPortIdSubtype = 3 (macAddress), `.3.1` lldpLocPortId = the
     6 MAC bytes, `.4.1` lldpLocPortDesc = "W5500 10/100 Ethernet" (what LLDP
     sends; LibreNMS matches local ports by these and by ifIndex).
   - Always present.
2. **lldpRemTable** (`1.0.8802.1.1.2.1.4.1.1`), one row while the device holds
   a neighbour, none otherwise. Index lldpRemTimeMark.lldpRemLocalPortNum.lldpRemIndex
   = **0.1.1** (our call: a fixed instance fits the agent's fixed MIB table;
   TimeFilter value 0 is valid and LibreNMS ignores the time mark; a changing
   time mark or index would need dynamic OIDs for no gain with one neighbour).
   Columns, values as the switch sent them:
   - `.4` lldpRemChassisIdSubtype (INTEGER), `.5` lldpRemChassisId (OCTET
     STRING, the raw bytes: 6 bytes for a MAC)
   - `.6` lldpRemPortIdSubtype, `.7` lldpRemPortId: the raw bytes, **except**
     when the switch sent a locally assigned Port ID (subtype 7) and a Port
     Description: then subtype 5 (interfaceName) and the Port Description
     (user's decision). LibreNMS labels the remote port with lldpRemPortId
     (`DiscoveryProtocols::remotePortLabel`); Junos sends "523" (its ifIndex)
     as Port ID and "ge-0/0/9.0" as description, and the user wants the page
     to say ge-0/0/9.0, as the status screen does.
   - `.8` lldpRemPortDesc, `.9` lldpRemSysName, `.10` lldpRemSysDesc
     (empty string if not sent)
   - `.11` lldpRemSysCapSupported, `.12` lldpRemSysCapEnabled (BITS = OCTET
     STRING of 2 bytes, as received; `00 00` if not sent)
   Index columns 1-3 are not-accessible and not served.
3. Both v1 and v2c; nothing here is Counter64, so v1 sees the same objects. A
   Get on a missing row: noSuchName (v1) / noSuchInstance (v2c), as for other
   objects; GetNext and GetBulk skip it.
4. The received values the tables need are kept by `lib/lldp` next to the text
   already kept for the screen: raw Chassis ID and Port ID bytes with their
   subtypes, System Description, capabilities. Cut to fixed lengths (our call:
   IDs 64 bytes, System Description 128 bytes); the screen text stays as today.
5. Verified in LibreNMS (the user's Docker install, `librenms/test`): run
   discovery for the device, its Neighbours page lists r169 / ge-0/0/9.0.
   Recorded in docs/LIBRENMS.md with the result.
6. LibreNMS's automatic adding of LLDP/CDP neighbours (`autodiscovery.xdp`,
   default on) is turned off in the user's install (user's decision), with
   `lnms config:set autodiscovery.xdp false` run as the `librenms` user in
   the container (stored in LibreNMS's database, survives restarts). Otherwise
   discovery may try to add the switch (`r169.rf.internal` resolves on this
   network). docs/LIBRENMS.md says why and how, as a recommendation.

## Non-goals

- No lldpRemManAddrTable: its index carries the address bytes (variable
  length), the agent's table has fixed OIDs, and the user's switch sends no
  management address. LibreNMS then shows the neighbour without an IP. (Can be
  added later.)
- No lldpLocalSystemData, lldpConfiguration, lldpStatistics, LLDP-V2-MIB.
- No alerts on port moves or a lost switch (the user only wants to see it).
- No change to LLDP on the wire or to the screen and web page.
- No LibreNMS code or definition changes (the YAML definition stays as is);
  the only LibreNMS change is the one setting in Goal 6.

## Anti-goals

- v1 behaviour of existing objects must not change. `1.0.8802...` sorts
  **before** `1.3.6.1...`: a walk from the root (`.1`) now begins with the
  LLDP objects, while net-snmp's default walk (mib-2, `1.3.6.1.2.1`) and the
  enterprise subtree are unchanged.
- Remote text is served as received (apart from the Port ID rule in Goal 2;
  it is not shown as HTML here), but never longer than the caps, and the
  agent must not read past what it keeps.
- RAM stays under the 50 % rule (now 45.0 %); the extra neighbour data is a
  few hundred bytes, static.

## Constraints

- Pure code in `lib/lldp` and `lib/snmp_agent`, host tested; the firmware fills
  the new `SnmpDeviceState` fields from the service under the driver lock
  (`src/snmp_service.cpp captureState`, `src/lldp_service.cpp`).
- Tests: native agent tests (Get/GetNext/GetBulk, v1 and v2c, present/absent
  row, walk order), `scripts/host_agent_check.sh` against real net-snmp tools
  (numeric OIDs, or LLDP-MIB copied from LibreNMS's `mibs/` if names are
  wanted: WSL's net-snmp has no LLDP-MIB), sanitizers, on-device tests, HIL:
  `snmpwalk` of `1.0.8802.1.1.2` on the device names the same switch and port
  as the status screen (opt-in `RFUPS_LLDP_SWITCH=1`).
- Known effect to document: a full `snmpwalk` from the root now starts with
  the LLDP objects. Check `test/hil/test_snmp.py`, `host_agent_check.sh` and
  the native walk tests that assume the first object is sysDescr.
- Docs: docs/MIB.md (new objects), README SNMP section, docs/LIBRENMS.md,
  CHANGELOG, TODO, docs/TESTING.md. Version 2.7.0.

## Left to the builder

- How the MIB table marks rows present (the existing `present()` hook is the
  likely fit), struct and field names, exact caps if RAM needs them smaller.
- Whether `host_agent_check.sh` uses numeric OIDs or a copied LLDP-MIB.
