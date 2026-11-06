#pragma once

/// Firmware version (semantic versioning), reported in sysDescr and at boot
#define FIRMWARE_VERSION "3.3.0"

/// Hardware the firmware is built for, reported as ENTITY-MIB entPhysicalHardwareRev.
/// Change it with the board (the carrier's revision is in hardware/pico-poe-mains-carrier/tools/generate.py)
#define HARDWARE_REVISION "W5500-EVB-Pico-PoE + carrier rev 1.1"
