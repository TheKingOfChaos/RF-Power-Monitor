// Stands in for the Pico SDK's generated board configuration when the boot
// program includes the SDK's register headers (hardware/structs/...). The
// boot program uses no SDK code and no board settings, only register layouts;
// the core's own config_autogen.h would pull in a board header (pico_w.h).
#pragma once
