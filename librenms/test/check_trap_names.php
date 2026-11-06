<?php
/**
 * Checks isPollingTrap() in snmptrap-agent-addr.php against every way snmptrapd can name the
 * device's mains and firmware traps: by number, with the current MIB, with an older MIB file loaded (a newer
 * trap by its parent), and with the pre-3.0 MIB (RF-UPS-MONITOR-MIB) still loaded after an update.
 * The handler script runs LibreNMS when included, so only the function is taken out of it.
 *
 *   php librenms/test/check_trap_names.php      (any PHP 8; prints PASS/FAIL, exit 1 on a failure)
 */

$src = file_get_contents(__DIR__ . '/snmptrap-agent-addr.php');
if (! preg_match('/function isPollingTrap\(.*?\n}\n/s', $src, $m)) {
    fwrite(STDERR, "isPollingTrap() not found\n");
    exit(1);
}
eval($m[0]);

$cases = [
    // by number
    ['.1.3.6.1.4.1.63050.0.1', true],
    ['SNMPv2-SMI::enterprises.63050.0.3', true],
    // current MIB
    ['RF-POWER-MONITOR-MIB::mainsPowerLost', true],
    ['RF-POWER-MONITOR-MIB::mainsPowerRestored', true],
    ['RF-POWER-MONITOR-MIB::mainsPowerLostTooLong', true],
    // pre-3.0 MIB still loaded (snmptrapd not restarted after the update)
    ['RF-UPS-MONITOR-MIB::mainsPowerLost', true],
    ['RF-UPS-MONITOR-MIB::mainsPowerLostTooLong', true],
    // a MIB older than the trap names it by its parent
    ['RF-UPS-MONITOR-MIB::rfUpsMonitorNotifications.3', true],
    ['RF-POWER-MONITOR-MIB::rfPowerMonitorNotifications.3', true],
    // firmware update traps
    ['.1.3.6.1.4.1.63050.0.4', true],
    ['SNMPv2-SMI::enterprises.63050.0.5', true],
    ['RF-POWER-MONITOR-MIB::firmwareUpdateKept', true],
    ['RF-POWER-MONITOR-MIB::firmwareRolledBack', true],
    ['RF-POWER-MONITOR-MIB::rfPowerMonitorNotifications.4', true],
    ['RF-UPS-MONITOR-MIB::rfUpsMonitorNotifications.5', true],
    // not mains traps
    ['SNMPv2-MIB::coldStart', false],
    ['SNMPv2-MIB::authenticationFailure', false],
    ['RF-POWER-MONITOR-MIB::rfPowerMonitorNotifications.6', false],
    ['.1.3.6.1.4.1.63050.0.6', false],
    ['.1.3.6.1.4.1.630500.0.1', false],
];

$failed = 0;
foreach ($cases as [$oid, $want]) {
    $got = isPollingTrap($oid);
    $ok = $got === $want;
    $failed += $ok ? 0 : 1;
    printf("%s  %-55s %s\n", $ok ? 'PASS' : 'FAIL', $oid, $want ? 'polls sensors' : 'ignored');
}
echo $failed ? "$failed failed\n" : "all passed\n";
exit($failed ? 1 : 0);
