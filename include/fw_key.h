// The public key firmware updates must be signed with (FW_PUBLIC_KEY, 32 bytes).
//
// Development builds trust the key in keys/dev (include/fw_dev_public_key.h).
// A build with RFPOWER_FW_FIELD_KEY defined (set by scripts/slot_build.py when
// RFPOWER_FW_KEY=field) trusts the field key instead, include/fw_public_key.h,
// which tools/fw_keygen.py writes next to secret/fw_signing_key.
#pragma once

#ifdef RFPOWER_FW_FIELD_KEY
#if __has_include("fw_public_key.h")
#include "fw_public_key.h"
#else
#error "Run tools/fw_keygen.py"
#endif
#else
#include "fw_dev_public_key.h"
#endif
