"""Helpers for the SNMP HIL tests: net-snmp tools in WSL and raw UDP from Windows."""

import os
import random
import socket
import struct
import subprocess
import sys
import time

WSL_DISTRO = os.environ.get("RFUPS_WSL_DISTRO", "Ubuntu-24.04")
SNMP_PORT = 161

# snmpget -v1 -c public 1.3.6.1.2.1.1.1.0, captured from net-snmp. Request id at bytes 17..20.
GET_SYSDESCR = bytes.fromhex(
    "30 29 02 01 00 04 06 70 75 62 6c 69 63 a0 1c 02 04 46 74 1e 94 02 01 00 02 01 00"
    " 30 0e 30 0c 06 08 2b 06 01 02 01 01 01 00 05 00")
REQUEST_ID_OFFSET = 17


def net_snmp(tool, host, *args, community="public", version="1", timeout=2, retries=0):
    """Run a net-snmp tool (snmpget, snmpwalk, ...) in WSL; return (exit code, output)."""
    cmd = [tool, "-v" + version, "-c", community, "-r", str(retries), "-t", str(timeout),
           "-On", "-M", "/dev/null", host] + list(args)
    if sys.platform == "win32":
        cmd = ["wsl", "-d", WSL_DISTRO, "--"] + cmd
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                            timeout=timeout * (retries + 1) + 30)
    return result.returncode, result.stdout


def raw_get(host, request_id, timeout=1.0, packet=None):
    """Send a GetRequest for sysDescr over UDP; return (response bytes or None, seconds)."""
    pkt = bytearray(packet if packet is not None else GET_SYSDESCR)
    if packet is None:
        struct.pack_into(">I", pkt, REQUEST_ID_OFFSET, request_id & 0xFFFFFFFF)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(timeout)
        start = time.perf_counter()
        s.sendto(bytes(pkt), (host, SNMP_PORT))
        try:
            data, _ = s.recvfrom(2048)
            return data, time.perf_counter() - start
        except socket.timeout:
            return None, time.perf_counter() - start


def response_request_id(resp):
    """Request id of a GetResponse to GET_SYSDESCR (community "public"), decoded from BER."""
    assert resp[0] == 0x30 and resp[13] == 0xA2, "not a GetResponse"
    pos = 13 + 2 if resp[14] < 0x80 else 13 + 2 + (resp[14] & 0x7F)  # skip PDU header
    assert resp[pos] == 0x02, "request-id is not an INTEGER"
    n = resp[pos + 1]
    return int.from_bytes(resp[pos + 2:pos + 2 + n], "big", signed=True)


def local_ip_towards(host):
    """This PC's address on the interface that reaches host (no packet is sent)."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect((host, SNMP_PORT))
        return s.getsockname()[0]


def _ber(data, pos):
    """Read one BER TLV at pos; return (tag, value bytes, next position)."""
    tag = data[pos]
    length = data[pos + 1]
    pos += 2
    if length & 0x80:
        n = length & 0x7F
        length = int.from_bytes(data[pos:pos + n], "big")
        pos += n
    return tag, data[pos:pos + length], pos + length


def _oid(value):
    arcs, v = [], 0
    for b in value:
        v = (v << 7) | (b & 0x7F)
        if not b & 0x80:
            arcs.append(v)
            v = 0
    first = arcs[0]
    head = [0, first] if first < 40 else ([1, first - 40] if first < 80 else [2, first - 80])
    return ".".join(map(str, head + arcs[1:]))


TRAP_OIDS_V2C = {
    "1.3.6.1.6.3.1.1.5.1": "coldStart",
    "1.3.6.1.6.3.1.1.5.5": "authenticationFailure",
    "1.3.6.1.4.1.63050.0.1": "mainsPowerLost",
    "1.3.6.1.4.1.63050.0.2": "mainsPowerRestored",
}
# snmpTrapAddress.0 (RFC 3584): the device's address in its v2c traps, as the v1 agent-addr
SNMP_TRAP_ADDRESS = "1.3.6.1.6.3.18.1.3.0"
TRAPS_V1 = {(0, 0): "coldStart", (4, 0): "authenticationFailure",
            (6, 1): "mainsPowerLost", (6, 2): "mainsPowerRestored"}


def _varbinds(vbl):
    """List of (oid, tag, value) from an encoded VarBindList."""
    out, q = [], 0
    while q < len(vbl):
        _, vb, q = _ber(vbl, q)
        _, name, r = _ber(vb, 0)
        vtag, value, r = _ber(vb, r)
        if vtag == 0x06:
            v = _oid(value)
        elif vtag == 0x40:
            v = ".".join(map(str, value))
        else:
            v = int.from_bytes(value, "big", signed=(vtag == 0x02))
        out.append((_oid(name), vtag, v))
    return out


def decode_trap(data):
    """Decode an SNMPv1 Trap-PDU or SNMPv2-Trap into a dict (raises on anything else).

    Both give: version (0 = v1, 1 = v2c), community, kind (coldStart, authenticationFailure,
    mainsPowerLost, mainsPowerRestored), timestamp (TimeTicks), agent (v1 agent-addr, v2c
    snmpTrapAddress.0, None if absent) and varbinds {oid: (tag, value)} without the v2c
    sysUpTime.0 / snmpTrapOID.0 header and without snmpTrapAddress.0.
    """
    tag, msg, _ = _ber(data, 0)
    assert tag == 0x30
    _, version, p = _ber(msg, 0)
    _, community, p = _ber(msg, p)
    tag, pdu, _ = _ber(msg, p)
    version = int.from_bytes(version, "big")
    result = {"version": version, "community": community.decode(errors="replace")}

    if tag == 0xA4:
        assert version == 0, "Trap-PDU in a non-v1 message"
        _, enterprise, p = _ber(pdu, 0)
        _, addr, p = _ber(pdu, p)
        _, generic, p = _ber(pdu, p)
        _, specific, p = _ber(pdu, p)
        _, ticks, p = _ber(pdu, p)
        _, vbl, p = _ber(pdu, p)
        generic, specific = int.from_bytes(generic, "big"), int.from_bytes(specific, "big")
        result.update({
            "enterprise": _oid(enterprise),
            "agent": ".".join(map(str, addr)),
            "generic": generic,
            "specific": specific,
            "kind": TRAPS_V1.get((generic, specific), "unknown"),
            "timestamp": int.from_bytes(ticks, "big"),
            "varbinds": {o: (t, v) for o, t, v in _varbinds(vbl)},
        })
    elif tag == 0xA7:
        assert version == 1, "SNMPv2-Trap in a non-v2c message"
        _, request_id, p = _ber(pdu, 0)
        _, _status, p = _ber(pdu, p)
        _, _index, p = _ber(pdu, p)
        _, vbl, p = _ber(pdu, p)
        vbs = _varbinds(vbl)
        assert vbs[0][0] == "1.3.6.1.2.1.1.3.0" and vbs[0][1] == 0x43, "first varbind must be sysUpTime.0"
        assert vbs[1][0] == "1.3.6.1.6.3.1.1.4.1.0" and vbs[1][1] == 0x06, "second must be snmpTrapOID.0"
        agent = None
        if vbs[-1][0] == SNMP_TRAP_ADDRESS:
            assert vbs[-1][1] == 0x40, "snmpTrapAddress.0 must be an IpAddress"
            agent = vbs.pop()[2]
        assert all(o != SNMP_TRAP_ADDRESS for o, _, _ in vbs), "snmpTrapAddress.0 must be the last varbind"
        result.update({
            "agent": agent,
            "requestId": int.from_bytes(request_id, "big", signed=True),
            "trapOid": vbs[1][2],
            "kind": TRAP_OIDS_V2C.get(vbs[1][2], "unknown"),
            "timestamp": vbs[0][2],
            "varbinds": {o: (t, v) for o, t, v in vbs[2:]},
        })
    else:
        raise AssertionError("not a trap PDU: 0x%02x" % tag)
    return result


class TrapListener:
    """Collects SNMP traps on UDP 162 in a background thread."""

    def __init__(self, port=162):
        import threading
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", port))
        self.sock.settimeout(0.2)
        self.traps = []
        self._lock = threading.Lock()
        self._stop = False
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self):
        while not self._stop:
            try:
                data, src = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                return
            try:
                trap = decode_trap(data)
            except Exception as e:  # keep malformed traps visible to the test
                trap = {"error": repr(e), "raw": data.hex(" ")}
            trap["source"] = src[0]
            trap["received"] = time.time()
            with self._lock:
                self.traps.append(trap)

    def clear(self):
        with self._lock:
            self.traps = []

    def wait_for(self, predicate, timeout):
        """Return the first trap matching predicate within timeout, else None."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self._lock:
                for t in self.traps:
                    if "error" not in t and predicate(t):
                        return t
            time.sleep(0.05)
        return None

    def matching(self, predicate):
        with self._lock:
            return [t for t in self.traps if "error" not in t and predicate(t)]

    def close(self):
        self._stop = True
        self.sock.close()
        self._thread.join(timeout=2)


def send_raw(host, payloads):
    """Fire-and-forget a list of UDP payloads at the agent."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        for p in payloads:
            s.sendto(p, (host, SNMP_PORT))


def garbage_packets(count, seed=1234):
    """Malformed packets: random bytes, truncations and single-byte corruptions of a real request."""
    rng = random.Random(seed)
    out = []
    for i in range(count):
        kind = i % 3
        if kind == 0:
            out.append(bytes(rng.randrange(256) for _ in range(rng.randrange(1, 200))))
        elif kind == 1:
            out.append(GET_SYSDESCR[:rng.randrange(1, len(GET_SYSDESCR))])
        else:
            p = bytearray(GET_SYSDESCR)
            p[rng.randrange(len(p))] = rng.randrange(256)
            out.append(bytes(p))
    return out
