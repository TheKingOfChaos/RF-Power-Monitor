"""POST /api/checkin: verify, record, decide the reply (wire format in protocol.py)."""

from . import protocol

BAD_AUTH = (401, b"status=badauth\n")
REPLAY = (409, b"status=replay\n")
BAD = (400, b"status=bad\n")


def handle_checkin(store, body: bytes, auth_header, now: float):
    """(HTTP status, reply body) for a check-in. Nothing is read from the body before
    its MAC is verified, and error replies carry no auth line (the device ignores them)."""
    key = store.site_key()
    if key is None or len(body) > protocol.BODY_MAX or not protocol.verify_mac(key, body, auth_header):
        return BAD_AUTH
    try:
        fields = protocol.parse_checkin(body)
    except ValueError:
        return BAD
    if store.nonce_seen(fields["serial"], fields["nonce"], now):
        return REPLAY
    store.upsert_checkin(fields, now)
    version = store.advance(fields, now)
    if version is None:
        lines = ["status=ok"]
    else:
        lines = ["status=update", "version=" + version, "path=/releases/%s.upd" % version]
    return 200, protocol.build_reply(key, fields["nonce"], lines)
