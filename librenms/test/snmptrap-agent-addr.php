#!/usr/bin/env php
<?php
/**
 * LibreNMS trap handler for Docker Desktop, mounted over /opt/librenms/snmptrap.php in the
 * snmptrapd container. Docker Desktop's port forwarding replaces the sender's address with its
 * own (172.18.0.1), so LibreNMS cannot find the device. The device's traps carry its address
 * in snmpTrapAddress.0 (v2c, firmware 2.5.1 and later) or in agent-addr, which snmptrapd shows
 * as snmpTrapAddress.0 too (v1): this puts that address in the first two lines (host name and
 * "UDP: [address]:port->..."), where LibreNMS looks for it, then hands the trap to LibreNMS as
 * snmptrap.php does. Traps without it pass through unchanged.
 *
 * LibreNMS only logs the RF-POWER-MONITOR-MIB traps; alerts come from the sensors. After a mains
 * trap (lost, restored, or lost for 60 s) this polls the device's sensors, which runs its alert
 * rules, so "State Sensor Critical" on "Mains lost over 60 s" opens about 60 s after mains goes and
 * recovers when it returns, instead of at the next 5-minute poll. The firmware traps (update kept,
 * rolled back) poll the "Firmware" sensors the same way.
 */

use LibreNMS\Util\Debug;

$init_modules = [];
require __DIR__ . '/includes/init.php';

$options = getopt('d::');
if (Debug::set(isset($options['d']))) {
    echo "DEBUG!\n";
}

/**
 * Put the agent address from snmpTrapAddress.0 in place of the forwarded sender address.
 *
 * @param  string  $text  trap text from snmptrapd
 * @return string the trap text, with the device's address when the trap has one
 */
function useAgentAddress(string $text): string
{
    if (! preg_match('/^\S*snmpTrapAddress\.0 "?([0-9.]+)"?\s*$/m', $text, $m)
        || filter_var($m[1], FILTER_VALIDATE_IP, FILTER_FLAG_IPV4) === false) {
        return $text;
    }
    $lines = explode(PHP_EOL, $text);
    $lines[0] = $m[1];
    if (isset($lines[1])) {
        $lines[1] = preg_replace('/\[[0-9.:a-fA-F]+]/', '[' . $m[1] . ']', $lines[1], 1);
    }

    return implode(PHP_EOL, $lines);
}

/**
 * Whether the trap is one that changes a sensor: mainsPowerLost, mainsPowerRestored, mainsPowerLostTooLong,
 * firmwareUpdateKept or firmwareRolledBack, by name or by number.
 *
 * @param  string  $trapOid  snmpTrapOID.0 as snmptrapd printed it
 * @return bool
 */
function isPollingTrap(string $trapOid): bool
{
    // RF-UPS-MONITOR-MIB is the module's name before 3.0.0: snmptrapd keeps it until restarted
    return preg_match('/RF-(UPS|POWER)-MONITOR-MIB::(mainsPower|firmwareUpdateKept|firmwareRolledBack)/', $trapOid) === 1
        || preg_match('/(\.1\.3\.6\.1\.4\.1\.|enterprises\.)63050\.0\.[12345]$/', $trapOid) === 1
        // snmptrapd started with an older MIB file names a newer trap by its parent
        || preg_match('/RF-(UPS|POWER)-MONITOR-MIB::rf(Ups|Power)MonitorNotifications\.[12345]$/', $trapOid) === 1;
}

$trap = new \LibreNMS\Snmptrap\Trap(useAgentAddress(stream_get_contents(STDIN)));
\LibreNMS\Snmptrap\Dispatcher::handle($trap);

// snmptrapd runs this as root; the poller writes RRD files and must run as librenms
$device = $trap->getDevice();
if ($device !== null && isPollingTrap($trap->getTrapOid())) {
    exec('s6-setuidgid librenms /opt/librenms/lnms device:poll ' . (int) $device->device_id . ' -m sensors -q');
}
