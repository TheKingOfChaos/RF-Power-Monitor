<?php

/**
 * LibreNMS plugin: a "Switch" box on the Overview page of RF-Power-Monitor devices.
 *
 * Shows the switch and port the device is plugged into, from the LLDP links that
 * LibreNMS's discovery-protocols module stores (read from the device's LLDP-MIB).
 * Install: copy this directory to <librenms>/app/Plugins/RFPowerMonitor (docs/LIBRENMS.md).
 */

namespace App\Plugins\RFPowerMonitor;

use App\Models\Device;
use App\Models\User;
use App\Plugins\Hooks\DeviceOverviewHook;

class DeviceOverview extends DeviceOverviewHook
{
    /**
     * Only RF-Power-Monitor devices get the box.
     */
    public function authorize(User $user, Device $device): bool
    {
        return $device->os === 'rfpowermonitor';
    }

    /**
     * The device's active LLDP links (normally one: the switch port) for the view.
     */
    public function data(Device $device): array
    {
        return [
            'title' => 'Switch',
            'device' => $device,
            'links' => $device->links()->where('active', 1)->with(['port', 'remoteDevice', 'remotePort'])->get(),
            'neighboursUrl' => url('device/' . $device->device_id . '/neighbours'),
        ];
    }
}
