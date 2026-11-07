#pragma once

/// Firmware version (semantic versioning), reported in sysDescr and at boot.
/// tools/fw_release.py --test-version builds test images with another one (update site HIL test)
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "3.6.0"
#endif

/// Hardware the firmware is built for, reported as ENTITY-MIB entPhysicalHardwareRev.
/// Change it with the board (the carrier's revision is in hardware/pico-poe-mains-carrier/tools/generate.py)
#define HARDWARE_REVISION "W5500-EVB-Pico-PoE + carrier rev 1.1"
