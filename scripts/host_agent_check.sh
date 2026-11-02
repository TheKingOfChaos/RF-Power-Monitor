#!/bin/bash
# Check lib/snmp_agent against the real net-snmp tools, without hardware.
#
# Builds test/host/host_agent.cpp with AddressSanitizer + UBSan, runs it on
# 127.0.0.1, and checks snmpget / snmpgetnext / snmpwalk / snmpset output.
# Run in WSL (needs g++ and the snmp package):
#   wsl -d Ubuntu-24.04 -- bash scripts/host_agent_check.sh
set -uo pipefail
cd "$(dirname "$0")/.."

PORT=16161
TARGET=127.0.0.1:$PORT
OUT=.pio/host_agent
mkdir -p "$OUT"

g++ -std=gnu++17 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I lib/snmp_ber/include -I lib/snmp_agent/include -I lib/if_stats/include \
    -I lib/lldp/include -I lib/netconfig/include \
    test/host/host_agent.cpp lib/snmp_ber/src/*.cpp lib/snmp_agent/src/*.cpp lib/if_stats/src/*.cpp \
    lib/lldp/src/*.cpp lib/netconfig/src/*.cpp \
    -o "$OUT/host_agent" || exit 2

"$OUT/host_agent" $PORT public > "$OUT/agent.log" 2>&1 &
AGENT=$!
trap 'kill $AGENT 2>/dev/null' EXIT
sleep 0.5

pass=0
fail=0
SNMP_OPTS="-v1 -c public -r0 -t1 -On -M /dev/null"

# check <name> <expected regex> <command...>
# Output lines are joined with spaces so a regex can span several lines.
check() {
    local name=$1 expect=$2
    shift 2
    local got
    got=$("$@" 2>&1)
    if echo "$got" | tr '\n' ' ' | sed 's/ *$//' | grep -Eq "$expect"; then
        echo "PASS  $name"
        pass=$((pass + 1))
    else
        echo "FAIL  $name"
        echo "      expected: $expect"
        echo "$got" | sed 's/^/      got: /'
        fail=$((fail + 1))
    fi
}

check "get sysDescr"            'STRING: "RF-Power-Monitor host test"' snmpget $SNMP_OPTS $TARGET 1.3.6.1.2.1.1.1.0
check "get sysObjectID"         'OID: \.1\.3\.6\.1\.4\.1\.63050$' snmpget $SNMP_OPTS $TARGET 1.3.6.1.2.1.1.2.0
check "get sysUpTime"           'Timeticks: \([0-9]+\)' snmpget $SNMP_OPTS $TARGET 1.3.6.1.2.1.1.3.0
check "get sysServices"         'INTEGER: 72' snmpget $SNMP_OPTS $TARGET 1.3.6.1.2.1.1.7.0
check "get mainsPowerStatus"    '63050\.1\.0 = INTEGER: 1' snmpget $SNMP_OPTS $TARGET 1.3.6.1.4.1.63050.1.0
check "get mainsPowerChanges"   '63050\.2\.0 = Counter32: 2' snmpget $SNMP_OPTS $TARGET 1.3.6.1.4.1.63050.2.0
check "get mainsPowerLastChange" '63050\.3\.0 = Timeticks: \(42\)' snmpget $SNMP_OPTS $TARGET 1.3.6.1.4.1.63050.3.0
NO_SUCH='Reason: \(noSuchName\) There is no such variable name in this MIB\. Failed object: '
check "get two varbinds"        '1\.3\.0 = Timeticks: \([0-9]+\).* \.1\.3\.6\.1\.4\.1\.63050\.1\.0 = INTEGER: 1$' \
      snmpget $SNMP_OPTS $TARGET 1.3.6.1.2.1.1.3.0 1.3.6.1.4.1.63050.1.0
check "get unknown is noSuchName" "${NO_SUCH}\.1\.3\.6\.1\.2\.1\.1\.99\.0" snmpget $SNMP_OPTS $TARGET 1.3.6.1.2.1.1.99.0
check "get 2nd unknown names it" "${NO_SUCH}\.1\.3\.6\.1\.9\.0" \
      snmpget $SNMP_OPTS $TARGET 1.3.6.1.2.1.1.1.0 1.3.6.1.9.0
check "getnext enterprise"      '63050\.1\.0 = INTEGER: 1' snmpgetnext $SNMP_OPTS $TARGET 1.3.6.1.4.1.63050
check "getnext past end"        "${NO_SUCH}\.1\.3\.6\.1\.6\.3\.1\.1\.6\.1\.0" snmpgetnext $SNMP_OPTS $TARGET 1.3.6.1.6.3.1.1.6.1.0
check "v1 walk all, 169 objects" '^169$' bash -c "snmpwalk $SNMP_OPTS $TARGET .1 | grep -c ' = '"
check "walk order"              '1\.1\.0.*1\.2\.0.*1\.3\.0.*1\.4\.0.*1\.5\.0.*1\.6\.0.*1\.7\.0.*63050\.1\.0.*63050\.2\.0.*63050\.3\.0' \
      bash -c "snmpwalk $SNMP_OPTS $TARGET .1 | cut -d' ' -f1 | tr '\n' ' '"
check "walk enterprise, 6 objects" '^6$' bash -c "snmpwalk $SNMP_OPTS $TARGET 1.3.6.1.4.1.63050 | grep -c ' = '"
check "set is rejected"         "${NO_SUCH}\.1\.3\.6\.1\.2\.1\.1\.5\.0" \
      snmpset $SNMP_OPTS $TARGET 1.3.6.1.2.1.1.5.0 s newname
check "sysName unchanged"       'STRING: "host-agent"' snmpget $SNMP_OPTS $TARGET 1.3.6.1.2.1.1.5.0
check "wrong community: no reply" 'Timeout' snmpget -v1 -c wrong -r0 -t1 -On -M /dev/null $TARGET 1.3.6.1.2.1.1.1.0
V2="-v2c -c public -r0 -t1 -On -M /dev/null"
check "v2c get sysDescr"        'STRING: "RF-Power-Monitor host test"' snmpget $V2 $TARGET 1.3.6.1.2.1.1.1.0
check "v2c noSuchObject"        '1\.99\.0 = No Such Object available on this agent at this OID' \
      snmpget $V2 $TARGET 1.3.6.1.2.1.1.99.0
check "v2c noSuchInstance"      '1\.1\.5 = No Such Instance currently exists at this OID' \
      snmpget $V2 $TARGET 1.3.6.1.2.1.1.1.5
check "v2c mixed get, no error" '1\.5\.0 = STRING: "host-agent".*1\.99\.0 = No Such Object' \
      snmpget $V2 $TARGET 1.3.6.1.2.1.1.5.0 1.3.6.1.2.1.1.99.0
check "v2c getnext past end"    '1\.1\.6\.1\.0 = No more variables left in this MIB View' \
      snmpgetnext $V2 $TARGET 1.3.6.1.6.3.1.1.6.1.0
check "v2c walk all, 177 objects" '^177$' bash -c "snmpwalk $V2 $TARGET .1 | grep ' = ' | grep -vc 'No more variables'"
check "v2c walk ends at view end" 'No more variables left in this MIB View' snmpwalk $V2 $TARGET .1
check "v2c walk order"          '1\.1\.0.*1\.7\.0.*63050\.1\.0.*63050\.3\.0' \
      bash -c "snmpwalk $V2 $TARGET .1 | cut -d' ' -f1 | tr '\n' ' '"
check "bulkwalk all, 177 objects" '^177$' bash -c "snmpbulkwalk $V2 $TARGET .1 | grep ' = ' | grep -vc 'No more variables'"
check "bulkwalk = walk"         '^same$' bash -c "diff <(snmpwalk $V2 $TARGET .1 | grep ' = ' | cut -d' ' -f1) <(snmpbulkwalk $V2 $TARGET .1 | grep ' = ' | cut -d' ' -f1) >/dev/null && echo same"
check "bulkget n1 r3"           '1\.1\.4\.0 = STRING.*1\.1\.1\.0 = STRING.*1\.1\.2\.0 = OID.*1\.1\.3\.0 = Timeticks' \
      snmpbulkget $V2 -Cn1 -Cr3 $TARGET 1.3.6.1.2.1.1.3.0 1.3.6.1.2.1.1
check "bulkget capped at 16"    '^16$' bash -c "snmpbulkget $V2 -Cn0 -Cr100 $TARGET .1 | grep -c ' = '"
check "bulkget reaches end"     '63050\.4\.0 = INTEGER.*No more variables left in this MIB View' \
      snmpbulkget $V2 -Cn0 -Cr100 $TARGET 1.3.6.1.4.1.63050.2.0

IFMIB="-v2c -c public -r0 -t1 -M +/usr/share/snmp/mibs -m IF-MIB"
check "ifDescr / ifType"        'ifDescr\.1 = STRING: W5500 10/100 Ethernet.*ifType\.1 = INTEGER: ethernetCsmacd\(6\)' \
      snmpget $IFMIB $TARGET IF-MIB::ifDescr.1 IF-MIB::ifType.1
check "ifPhysAddress"           'ifPhysAddress\.1 = STRING: 2:0:0:0:0:1' snmpget $IFMIB $TARGET IF-MIB::ifPhysAddress.1
check "ifOperStatus up, 100M"   'ifOperStatus\.1 = INTEGER: up\(1\).*ifSpeed\.1 = Gauge32: 100000000.*ifHighSpeed\.1 = Gauge32: 100' \
      snmpget $IFMIB $TARGET IF-MIB::ifOperStatus.1 IF-MIB::ifSpeed.1 IF-MIB::ifHighSpeed.1
check "ifInOctets wraps, ifHC not" 'ifInOctets\.1 = Counter32: 705032704.*ifHCInOctets\.1 = Counter64: 5000000000' \
      snmpget $IFMIB $TARGET IF-MIB::ifInOctets.1 IF-MIB::ifHCInOctets.1
check "receive drops and errors" 'ifInDiscards\.1 = Counter32: 3.*ifInErrors\.1 = Counter32: 2.*ifInUnknownProtos\.1 = Counter32: 17' \
      snmpget $IFMIB $TARGET IF-MIB::ifInDiscards.1 IF-MIB::ifInErrors.1 IF-MIB::ifInUnknownProtos.1
check "v1: Counter64 absent"    'noSuchName' snmpget $SNMP_OPTS $TARGET 1.3.6.1.2.1.31.1.1.1.6.1
check "v1 ifX walk, no Counter64" '^0 11$' bash -c "o=\$(snmpwalk $SNMP_OPTS $TARGET 1.3.6.1.2.1.31); echo \$(echo \"\$o\" | grep -c Counter64) \$(echo \"\$o\" | grep -c ' = ')"
check "v2c ifX walk, 19 columns" '^19$' bash -c "snmpwalk $V2 $TARGET 1.3.6.1.2.1.31 | grep ' = ' | grep -vc 'No more'"
check "cpuTemperature"          '63050\.4\.0 = INTEGER: 250' snmpget $V2 $TARGET 1.3.6.1.4.1.63050.4.0
check "ambientTemperature"      '63050\.5\.0 = INTEGER: 215' snmpget $V2 $TARGET 1.3.6.1.4.1.63050.5.0
check "mainsPowerAlarm"         '63050\.6\.0 = INTEGER: 0' snmpget $SNMP_OPTS $TARGET 1.3.6.1.4.1.63050.6.0

# SNMPv2-MIB (RFC 3418): sysORTable, snmp group, snmpSetSerialNo, with net-snmp's names
SNMPV2="-v2c -c public -r0 -t1 -M +/usr/share/snmp/mibs -m SNMPv2-MIB"
check "sysORTable rows"         'sysORID\.1 = OID: SNMPv2-MIB::snmpMIB.*sysORDescr\.1 = STRING: SNMPv2-MIB.*sysORDescr\.9 = STRING: RF-POWER-MONITOR-MIB.*sysORUpTime\.9 = Timeticks: \(0\)' \
      bash -c "snmpwalk $SNMPV2 $TARGET SNMPv2-MIB::sysORTable | tr '\n' ' '"
check "sysORTable, 27 objects"  '^27$' bash -c "snmpwalk $SNMPV2 $TARGET SNMPv2-MIB::sysORTable | grep -c ' = '"
check "snmp group, 30 objects"  '^30$' bash -c "snmpwalk $SNMPV2 $TARGET SNMPv2-MIB::snmp | grep ' = ' | grep -vc 'No more'"
check "snmpInPkts counts"       'snmpInPkts\.0 = Counter32: [1-9][0-9]*' snmpget $SNMPV2 $TARGET SNMPv2-MIB::snmpInPkts.0
check "bad community counted"   'snmpInBadCommunityNames\.0 = Counter32: [1-9]' snmpget $SNMPV2 $TARGET SNMPv2-MIB::snmpInBadCommunityNames.0
check "snmpEnableAuthenTraps"   'snmpEnableAuthenTraps\.0 = INTEGER: enabled\(1\)' snmpget $SNMPV2 $TARGET SNMPv2-MIB::snmpEnableAuthenTraps.0
check "snmpSetSerialNo"         'snmpSetSerialNo\.0 = INTEGER: 0' snmpget $SNMPV2 $TARGET SNMPv2-MIB::snmpSetSerialNo.0

# HOST-RESOURCES-MIB and ENTITY-MIB with net-snmp's names (the host agent reports fixed values)
HRMIB="-v2c -c public -r0 -t1 -M +/usr/share/snmp/mibs -m HOST-RESOURCES-MIB:HOST-RESOURCES-TYPES"
check "hrStorage RAM"           'hrStorageType\.1 = OID: HOST-RESOURCES-TYPES::hrStorageRam.*hrStorageDescr\.1 = STRING: RAM.*hrStorageAllocationUnits\.1 = INTEGER: 1024 Bytes.*hrStorageSize\.1 = INTEGER: 256.*hrStorageUsed\.1 = INTEGER: 128' \
      bash -c "snmpwalk $HRMIB $TARGET HOST-RESOURCES-MIB::hrStorageTable | grep '\.1 = ' | tr '\n' ' '"
check "hrStorage LittleFS"      'hrStorageType\.2 = OID: HOST-RESOURCES-TYPES::hrStorageFlashMemory.*hrStorageSize\.2 = INTEGER: 256.*hrStorageUsed\.2 = INTEGER: 16' \
      bash -c "snmpwalk $HRMIB $TARGET HOST-RESOURCES-MIB::hrStorageTable | grep '\.2 = ' | tr '\n' ' '"
check "hrProcessorLoad"         'hrProcessorLoad\.768 = INTEGER: 7.*hrProcessorLoad\.769 = INTEGER: 1' \
      bash -c "snmpwalk $HRMIB $TARGET HOST-RESOURCES-MIB::hrProcessorLoad | tr '\n' ' '"
check "hrDevice processors"     'hrDeviceType\.768 = OID: HOST-RESOURCES-TYPES::hrDeviceProcessor.*hrDeviceDescr\.769 = STRING: RP2040 core 1 \(mains input\).*hrDeviceStatus\.769 = INTEGER: running\(2\)' \
      bash -c "snmpwalk $HRMIB $TARGET HOST-RESOURCES-MIB::hrDeviceTable | tr '\n' ' '"
check "hrMemorySize"            'hrMemorySize\.0 = INTEGER: 256 KBytes' snmpget $HRMIB $TARGET HOST-RESOURCES-MIB::hrMemorySize.0
# ENTITY-MIB numerically: WSL's ENTITY-MIB needs IANA-ENTITY-MIB, which snmp-mibs-downloader lacks
ENTP=1.3.6.1.2.1.47.1.1.1.1
check "entPhysicalTable row"    '5\.1 = INTEGER: 3.*8\.1 = STRING: "W5500-EVB-Pico-PoE \+ carrier rev 1\.1".*9\.1 = STRING: "3\.1\.0".*11\.1 = STRING: "E6614103E7452D2F".*13\.1 = STRING: "RF-Power-Monitor".*16\.1 = INTEGER: 2' \
      bash -c "snmpwalk $V2 $TARGET $ENTP | tr '\n' ' '"
check "entPhysicalTable, 15 columns" '^15$' bash -c "snmpwalk $V2 $TARGET $ENTP | grep -c ' = '"
check "entPhysicalMfgName UTF-8" '^4b656e6e65746820466f6c69204ac3b87267656e73656e$' \
      bash -c "snmpget -v2c -c public -r0 -t1 -On -Ox -M /dev/null $TARGET 1.3.6.1.2.1.47.1.1.1.1.12.1 | sed 's/.*Hex-STRING: //' | tr -d ' \n' | tr 'A-F' 'a-f'"

# LLDP-MIB (numeric: WSL's net-snmp has no LLDP-MIB); the host agent holds an EX2200-C neighbour
LLDP=1.0.8802.1.1.2.1
check "v2c LLDP-MIB walk, 12 objects" '^12$' bash -c "snmpwalk $V2 $TARGET 1.0.8802 | grep ' = ' | grep -vc 'No more'"
check "v1 LLDP-MIB walk, 12 objects" '^12$' bash -c "snmpwalk $SNMP_OPTS $TARGET 1.0.8802 | grep -c ' = '"
check "lldpLocPortTable"        '3\.7\.1\.2\.1 = INTEGER: 3.*3\.7\.1\.3\.1 = Hex-STRING: 02 00 00 00 00 01.*3\.7\.1\.4\.1 = STRING: "W5500 10/100 Ethernet"' \
      bash -c "snmpwalk $V2 $TARGET $LLDP.3.7 | tr '\n' ' '"
check "lldpRemTable ids"        '4\.1\.1\.4\.0\.1\.1 = INTEGER: 4.*5\.0\.1\.1 = Hex-STRING: F8 C0 01 C7 90 C0.*6\.0\.1\.1 = INTEGER: 5.*7\.0\.1\.1 = STRING: "ge-0/0/9\.0"' \
      bash -c "snmpwalk $V2 $TARGET $LLDP.4.1 | tr '\n' ' '"
check "lldpRemTable names"      '8\.0\.1\.1 = STRING: "ge-0/0/9\.0".*9\.0\.1\.1 = STRING: "r169".*10\.0\.1\.1 = STRING: "Juniper Networks, Inc\. ex2200-c-12p-2g".*11\.0\.1\.1 = Hex-STRING: 28 00.*12\.0\.1\.1 = Hex-STRING: 28 00' \
      bash -c "snmpwalk $V2 $TARGET $LLDP.4.1 | tr '\n' ' '"
check "lldpRemSysName other row" '9\.0\.1\.2 = No Such Instance' snmpget $V2 $TARGET $LLDP.4.1.1.9.0.1.2
check "v1 lldpRemSysName"       '9\.0\.1\.1 = STRING: "r169"' snmpget $SNMP_OPTS $TARGET $LLDP.4.1.1.9.0.1.1
check "mib-2 walk starts at sysDescr" '^\.1\.3\.6\.1\.2\.1\.1\.1\.0$' \
      bash -c "snmpwalk $V2 $TARGET 1.3.6.1.2.1 | head -1 | cut -d' ' -f1"
check "v2c set notWritable"     'notWritable.*Failed object: \.1\.3\.6\.1\.2\.1\.1\.5\.0' \
      snmpset $V2 $TARGET 1.3.6.1.2.1.1.5.0 s newname
check "v2c set noCreation"      'noCreation.*Failed object: \.1\.3\.6\.1\.2\.1\.1\.99\.0' \
      snmpset $V2 $TARGET 1.3.6.1.2.1.1.99.0 s x
check "v2c wrong community"     'Timeout' snmpget -v2c -c wrong -r0 -t1 -On -M /dev/null $TARGET 1.3.6.1.2.1.1.1.0
check "v3: no reply"            'Timeout|No Response' \
      snmpget -v3 -l noAuthNoPriv -u nobody -r0 -t1 -On -M /dev/null $TARGET 1.3.6.1.2.1.1.1.0
check "agent still alive"       'STRING' snmpget $SNMP_OPTS $TARGET 1.3.6.1.2.1.1.1.0

# MIB file: needs the standard IETF MIBs (apt install snmp-mibs-downloader)
MIBDIRS="+$(pwd)/mibs"
if snmptranslate -M "$MIBDIRS" -m SNMPv2-MIB -On SNMPv2-MIB::sysDescr >/dev/null 2>&1; then
    # -PW reports every MIB warning; the standard MIBs have plenty, so only
    # warnings that point at our file count
    check "MIB loads without warnings" '^warnings=0$' \
          bash -c "echo warnings=\$(snmptranslate -M '$MIBDIRS' -m RF-POWER-MONITOR-MIB -PW -On RF-POWER-MONITOR-MIB::mainsPowerStatus 2>&1 >/dev/null | grep -c RF-POWER-MONITOR-MIB)"
    check "MIB describes all objects" 'mainsPowerStatus.*mainsPowerChanges.*mainsPowerLastChange' \
          snmptranslate -M "$MIBDIRS" -m +RF-POWER-MONITOR-MIB -Tp RF-POWER-MONITOR-MIB::rfPowerMonitor
    check "MIB OID of mainsPowerStatus" '^\.1\.3\.6\.1\.4\.1\.63050\.1$' \
          snmptranslate -M "$MIBDIRS" -m +RF-POWER-MONITOR-MIB -On RF-POWER-MONITOR-MIB::mainsPowerStatus
    check "MIB notification mainsPowerLost" '^\.1\.3\.6\.1\.4\.1\.63050\.0\.1$' \
          snmptranslate -M "$MIBDIRS" -m +RF-POWER-MONITOR-MIB -On RF-POWER-MONITOR-MIB::mainsPowerLost
    check "MIB notification mainsPowerRestored" '^\.1\.3\.6\.1\.4\.1\.63050\.0\.2$' \
          snmptranslate -M "$MIBDIRS" -m +RF-POWER-MONITOR-MIB -On RF-POWER-MONITOR-MIB::mainsPowerRestored
    check "MIB notification mainsPowerLostTooLong" '^\.1\.3\.6\.1\.4\.1\.63050\.0\.3$' \
          snmptranslate -M "$MIBDIRS" -m +RF-POWER-MONITOR-MIB -On RF-POWER-MONITOR-MIB::mainsPowerLostTooLong
    check "MIB mainsPowerAlarm by name" 'RF-POWER-MONITOR-MIB::mainsPowerAlarm\.0 = INTEGER: none\(0\)' \
          snmpget -v1 -c public -r0 -t1 -M "$MIBDIRS" -m +RF-POWER-MONITOR-MIB $TARGET 1.3.6.1.4.1.63050.6.0
    check "MIB cpuTemperature with units" 'RF-POWER-MONITOR-MIB::cpuTemperature\.0 = INTEGER: 250 0\.1 degrees Celsius' \
          snmpget -v2c -c public -r0 -t1 -M "$MIBDIRS" -m +RF-POWER-MONITOR-MIB $TARGET 1.3.6.1.4.1.63050.4.0
    check "MIB ambientTemperature with units" 'RF-POWER-MONITOR-MIB::ambientTemperature\.0 = INTEGER: 215 0\.1 degrees Celsius' \
          snmpget -v2c -c public -r0 -t1 -M "$MIBDIRS" -m +RF-POWER-MONITOR-MIB $TARGET 1.3.6.1.4.1.63050.5.0
    check "MIB names in walk"       'RF-POWER-MONITOR-MIB::mainsPowerStatus\.0 = INTEGER: present\(1\)' \
          snmpget -v1 -c public -r0 -t1 -M "$MIBDIRS" -m +RF-POWER-MONITOR-MIB $TARGET 1.3.6.1.4.1.63050.1.0
else
    echo "SKIP  MIB checks (standard MIBs missing: sudo apt install snmp-mibs-downloader)"
fi

if ! kill -0 $AGENT 2>/dev/null; then
    echo "FAIL  agent crashed:"
    cat "$OUT/agent.log"
    fail=$((fail + 1))
fi

echo "$pass passed, $fail failed"
[ $fail -eq 0 ]
