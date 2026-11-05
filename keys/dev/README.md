# Development signing key

Public development key, CI and tests only. The private half is committed on purpose:
firmware signed with it is not trusted by devices running field firmware.
The field key lives in `secret/` (gitignored); create it with `tools/fw_keygen.py`.
