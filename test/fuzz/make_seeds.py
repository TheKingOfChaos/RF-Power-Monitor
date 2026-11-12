#!/usr/bin/env python3
"""Write the starting inputs (seeds) for the fuzz harnesses in test/fuzz.

    python3 test/fuzz/make_seeds.py <out dir>

Writes <out dir>/<harness>/<name> for every harness. Each seed is a well-formed
input in the harness's format (see the comment at the top of each fuzz_*.cpp),
so the fuzzer starts from real messages instead of random bytes. scripts/fuzz.sh
calls this; the seeds are generated, not committed.
"""
import json
import os
import struct
import sys

MAC = bytes([0x1E, 0xB2, 0xCF, 0x4E, 0xA2, 0x2C])
SERVER_MAC = bytes([0xf8, 0xc0, 0x01, 0xc7, 0x90, 0xc0])


# ---- BER (SNMP) ----

def ber_len(n):
    if n < 0x80:
        return bytes([n])
    b = n.to_bytes((n.bit_length() + 7) // 8, "big")
    return bytes([0x80 | len(b)]) + b


def tlv(tag, content):
    return bytes([tag]) + ber_len(len(content)) + content


def ber_int(v):
    n = 1
    while not (-(1 << (8 * n - 1)) <= v < (1 << (8 * n - 1))):
        n += 1
    return tlv(0x02, v.to_bytes(n, "big", signed=True))


def ber_oid(text):
    arcs = [int(a) for a in text.strip(".").split(".")]
    out = bytearray([arcs[0] * 40 + arcs[1]])
    for a in arcs[2:]:
        chunk = [a & 0x7F]
        a >>= 7
        while a:
            chunk.append(0x80 | (a & 0x7F))
            a >>= 7
        out += bytes(reversed(chunk))
    return tlv(0x06, bytes(out))


def snmp_request(version, community, pdu, oids, non_repeaters=0, max_rep=0, set_value=None):
    binds = b"".join(tlv(0x30, ber_oid(o) + (set_value if set_value else b"\x05\x00")) for o in oids)
    body = ber_int(4242) + ber_int(non_repeaters) + ber_int(max_rep) + tlv(0x30, binds)
    return tlv(0x30, ber_int(version) + tlv(0x04, community.encode()) + tlv(pdu, body))


def snmp_seeds():
    sysdescr = "1.3.6.1.2.1.1.1.0"
    return {
        "v1_get_sysdescr": snmp_request(0, "public", 0xA0, [sysdescr]),
        "v1_getnext_root": snmp_request(0, "public", 0xA1, ["1.3"]),
        "v1_get_many": snmp_request(0, "public", 0xA0, ["1.3.6.1.2.1.1.3.0", "1.3.6.1.2.1.2.2.1.10.1",
                                                        "1.3.6.1.4.1.63050.1.1.0"]),
        "v1_set": snmp_request(0, "public", 0xA3, ["1.3.6.1.2.1.1.5.0"], set_value=tlv(0x04, b"x")),
        "v1_bad_community": snmp_request(0, "privat", 0xA0, [sysdescr]),
        "v2c_get_missing": snmp_request(1, "public", 0xA0, ["1.3.6.1.2.1.1.99.0"]),
        "v2c_getnext_lldp": snmp_request(1, "public", 0xA1, ["1.0.8802.1.1.2.1.4.1.1"]),
        "v2c_getbulk_ifmib": snmp_request(1, "public", 0xA5, ["1.3.6.1.2.1.1", "1.3.6.1.2.1.2.2.1"], 1, 10),
        "v2c_getbulk_ip": snmp_request(1, "public", 0xA5, ["1.3.6.1.2.1.4.34"], 0, 20),
        "v2c_getbulk_host": snmp_request(1, "public", 0xA5, ["1.3.6.1.2.1.25"], 0, 30),
        "v2c_getbulk_end": snmp_request(1, "public", 0xA5, ["1.3.6.1.6.3.1.1.6.1.0"], 0, 5),
        "v3_refused": tlv(0x30, ber_int(3) + tlv(0x30, ber_int(1))),
    }


# ---- HTTP ----

def form_post(path, body, cookie=True):
    head = "POST %s HTTP/1.1\r\nHost: d\r\n" % path
    if cookie:
        head += "Cookie: theme=dark; sid=@S\r\n"
    head += "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: %d\r\n\r\n" % len(body)
    return (head + body).encode()


def pieces(raw, size):
    """[length byte][bytes] pieces, as fuzz_http_request.cpp and fuzz_dhcp6.cpp read them"""
    out = bytearray()
    for i in range(0, len(raw), size):
        part = raw[i:i + size]
        out += bytes([len(part)]) + part
    return bytes(out)


def http_requests():
    login = form_post("/login", "user=admin&password=goodpasswd1", cookie=False)
    return {
        "get_root": b"GET / HTTP/1.1\r\nHost: d\r\n\r\n",
        "get_query": b"GET /firmware?x=1%20 HTTP/1.1\r\nHost: d\r\nCookie: sid=abc\r\n\r\n",
        "login": login,
        "upload": (b"POST /firmware/upload HTTP/1.1\r\nHost: d\r\nX-CSRF-Token: @C\r\nCookie: sid=@S\r\n"
                   b"Content-Length: 1000\r\n\r\nRFUP\x01\x00\x00\x00"),
        "too_large": b"POST /network HTTP/1.1\r\nContent-Length: 99999\r\n\r\n",
        "head": b"HEAD / HTTP/1.0\r\n\r\n",
    }


def web_seeds():
    flags_in = bytes([1, 0, 0])  # log in, no switch name, at once
    seeds = {}
    for name, body in [
        ("network_static", "csrf=@C&mode=static&ip=10.0.0.5&mask=255.255.255.0&gateway=10.0.0.1"),
        ("network_dhcp", "csrf=@C&mode=dhcp"),
        ("ipv6", "csrf=@C&ipv6=fd00%3A%3A5&ipv6gw=fd00%3A%3A1"),
        ("community", "csrf=@C&community=rfit"),
        ("hostname", "csrf=@C&hostname=rack-4&contact=%22ops%22+%3Cops%40example.com%3E&location=Rack+4"),
        ("timelog", "csrf=@C&ntp=pool.ntp.org&syslog=10.132.0.45%3A514"),
        ("updateserver", "csrf=@C&url=http%3A%2F%2F10.132.0.45%3A8074&fromdhcp=1&key=" + "ab" * 32),
        ("updateserver_clear", "csrf=@C&url=&clearkey=1"),
        ("disable", "csrf=@C&confirm=1"),
        ("reboot", "csrf=@C"),
        ("switch", "csrf=@C"),
        ("logout", "csrf=@C"),
        ("bad_csrf", "csrf=0000"),
    ]:
        path = {"network_static": "/network", "network_dhcp": "/network", "updateserver_clear": "/updateserver",
                "switch": "/firmware/switch", "bad_csrf": "/community"}.get(name, "/" + name)
        seeds[name] = flags_in + form_post(path, body)
    seeds["status_page"] = flags_in + b"GET / HTTP/1.1\r\nCookie: sid=@S\r\n\r\n"
    seeds["firmware_page"] = flags_in + b"GET /firmware HTTP/1.1\r\nCookie: sid=@S\r\n\r\n"
    seeds["firmware_status"] = bytes([1 | 4 | 8, 0, 0]) + b"GET /firmware/status HTTP/1.1\r\nCookie: sid=@S\r\n\r\n"
    switch = b'r169 <script>"&\'</script>, port ge-0/0/9'
    seeds["status_switch_name"] = bytes([1, len(switch)]) + switch + b"\x00GET / HTTP/1.1\r\nCookie: sid=@S\r\n\r\n"
    seeds["expired"] = bytes([1, 0, 200]) + b"GET / HTTP/1.1\r\nCookie: sid=@S\r\n\r\n"
    seeds["login_wrong"] = bytes([0, 0, 0]) + form_post("/login", "user=admin&password=wrong", cookie=False)
    seeds["css"] = bytes([0, 0, 0]) + b"GET /static/bootstrap-5.3.3.min.css HTTP/1.1\r\n\r\n"
    seeds["upload"] = bytes([1, 0, 0]) + http_requests()["upload"]
    seeds["saves_fail"] = bytes([1 | 2, 0, 0]) + form_post("/community", "csrf=@C&community=x")
    return seeds


# ---- firmware update file ----

FW_MAGIC = 0x57464652
UPD_MAGIC = 0x50554652
KEY_ID = bytes(range(1, 9))


def slot_header(slot, image_len, version=b"3.6.2"):
    h = bytearray(b"\x00" * 64)  # signature (not checked by the receiver)
    h += struct.pack("<IIII", FW_MAGIC, 1, slot, image_len)
    h += version.ljust(16, b"\x00") + KEY_ID
    return bytes(h.ljust(256, b"\x00"))


def update_file(image_len=300):
    blobs = [slot_header(s, image_len) + bytes((i * 7) & 0xFF for i in range(image_len)) for s in (0, 1)]
    off0 = 64
    off1 = off0 + len(blobs[0])
    head = struct.pack("<II", UPD_MAGIC, 1) + b"\x00" * 16 + struct.pack("<IIII", off0, off1, len(blobs[0]),
                                                                        len(blobs[1]))
    return head.ljust(64, b"\x00") + blobs[0] + blobs[1]


def boot_record():
    r = struct.pack("<II", 0x52464252, 7) + bytes([0, 1, 1, 1]) + b"3.6.1".ljust(16, b"\x00")
    return r.ljust(64, b"\x00")  # the harness fixes the CRC


def fw_seeds():
    f = update_file()
    return {
        "slot_a": bytes([0, 64]) + f,
        "slot_b_expected": bytes([1 | 2, 200]) + f,
        "one_short": bytes([2 << 2, 0]) + f,
        "announced_huge": bytes([3 << 2, 255]) + f[:64],
        "slot_header": bytes([0, 0]) + slot_header(0, 1000),
        "boot_record": bytes([0, 0]) + boot_record(),
    }


# ---- Ethernet frames ----

def ipv4_udp(src_ip, dst_ip, sport, dport, payload):
    udp = struct.pack("!HHHH", sport, dport, 8 + len(payload), 0) + payload
    ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(udp), 1, 0, 64, 17, 0, bytes(src_ip), bytes(dst_ip))
    return MAC + SERVER_MAC + b"\x08\x00" + ip + udp  # checksums: the harness makes them valid


def dhcp_ack(options):
    b = bytearray(236)
    b[0], b[1], b[2] = 2, 1, 6
    b[4:8] = b"\x12\x34\x56\x78"
    b[16:20] = bytes([10, 132, 0, 77])
    b[28:34] = MAC
    opts = bytearray(b"\x63\x82\x53\x63")
    for code, value in options:
        opts += bytes([code, len(value)]) + value
    opts += b"\xff"
    return ipv4_udp([10, 132, 0, 1], [10, 132, 0, 77], 67, 68, bytes(b) + bytes(opts))


def dhcp_seeds():
    base = [(53, b"\x05"), (54, bytes([10, 132, 0, 1])), (1, bytes([255, 255, 255, 0])), (51, b"\x00\x00\x0e\x10")]
    request = bytearray(dhcp_ack([(53, b"\x03"), (55, bytes([1, 3, 6, 42])), (0, b""), (0, b"")]))
    request[14 + 28] = 1  # op BOOTREQUEST
    return {
        "ack_all": b"\x00" + dhcp_ack(base + [(224, bytes([10, 132, 0, 45])), (225, b"http://10.132.0.45:8074"),
                                              (7, bytes([10, 132, 0, 45, 10, 0, 0, 1])), (42, bytes([10, 0, 0, 2]))]),
        "ack_plain": b"\x00" + dhcp_ack(base),
        "offer": b"\x00" + dhcp_ack([(53, b"\x02"), (224, bytes([1, 2, 3, 4]))]),
        "bad_checksums": b"\x01" + dhcp_ack(base + [(224, bytes([10, 132, 0, 45]))]),
        "request": b"\x00" + bytes(request),
    }


def dhcp6_option(code, data):
    return struct.pack("!HH", code, len(data)) + data


def dhcp6_message(msg_type, extra=b""):
    duid_client = struct.pack("!HH", 3, 1) + MAC
    duid_server = struct.pack("!HHI", 1, 1, 0x2A2B2C2D) + SERVER_MAC
    addr = bytes.fromhex("fd16d8334f0e3ece0000000000000249")
    iaaddr = dhcp6_option(5, addr + struct.pack("!II", 3600, 7200))
    ia = dhcp6_option(3, struct.pack("!III", 1, 1800, 2880) + iaaddr)
    ntp = dhcp6_option(31, dhcp6_option(1, bytes.fromhex("20010db8000000000000000000000123")))
    return (bytes([msg_type]) + b"\x00\x00\x01" + dhcp6_option(1, duid_client) + dhcp6_option(2, duid_server) + ia
            + dhcp6_option(7, b"\xff") + ntp + extra)


def ipv6_frame(next_header, payload, src=None, hop=64):
    src = src or bytes.fromhex("fe80000000000000fac001fffec790c0")
    dst = bytes.fromhex("fe800000000000001cb2cffffe4ea22c")
    ip = struct.pack("!IHBB", 0x60000000, len(payload), next_header, hop) + src + dst
    return MAC + SERVER_MAC + b"\x86\xdd" + ip + payload


def dhcp6_seeds():
    advertise = dhcp6_message(2)
    reply = dhcp6_message(7)
    framed = ipv6_frame(17, struct.pack("!HHHH", 547, 546, 8 + len(reply), 0) + reply)
    ra = ipv6_frame(58, bytes([134, 0, 0, 0, 64, 0xC0, 0x07, 0x08]) + b"\x00" * 8, hop=255)
    return {
        "advertise_reply": b"\x00" + pieces(advertise, 255) + pieces(reply, 255),
        "exchange_renew": b"\x00" + pieces(advertise, 255) + pieces(reply, 255) + pieces(reply, 255),
        "status_nobinding": b"\x00" + pieces(dhcp6_message(7, dhcp6_option(13, b"\x00\x03no binding")), 255),
        "framed_reply": b"\x00" + bytes([len(framed)]) + framed,
        "router_advert": b"\x00" + bytes([len(ra)]) + ra,
    }


def lldp_tlv(t, data):
    return struct.pack("!H", (t << 9) | len(data)) + data


def lldp_seeds():
    tlvs = (lldp_tlv(1, b"\x04" + SERVER_MAC) + lldp_tlv(2, b"\x07523") + lldp_tlv(3, b"\x00\x78")
            + lldp_tlv(4, b"ge-0/0/9.0") + lldp_tlv(5, b"r169")
            + lldp_tlv(6, b"Juniper Networks, Inc. ex2200-c-12p-2g Ethernet Switch")
            + lldp_tlv(7, b"\x00\x14\x00\x14") + lldp_tlv(8, b"\x05\x01" + bytes([10, 132, 0, 2]) + b"\x02\x00\x00\x00\x01\x00")
            + lldp_tlv(127, b"\x00\x12\x0f\x01\x03\x6c\x03\x00\x10") + lldp_tlv(0, b""))
    head = bytes([0x01, 0x80, 0xc2, 0x00, 0x00, 0x0e]) + SERVER_MAC + b"\x88\xcc"
    return {
        "juniper": head + tlvs,
        "mac_port": head + lldp_tlv(1, b"\x04" + SERVER_MAC) + lldp_tlv(2, b"\x03" + MAC) + lldp_tlv(3, b"\x00\x00")
        + lldp_tlv(0, b""),
    }


# ---- update site ----

def update_client_seeds():
    def resp(body, status="200 OK"):
        return ("HTTP/1.1 %s\r\nContent-Type: text/plain\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s"
                % (status, len(body), body)).encode()
    return {
        "ok": bytes([3, 0]) + resp("status=ok\n"),
        "update": bytes([3, 16]) + resp("status=update\nversion=3.6.2\npath=/fw/3.6.2.upd\n"),
        "badauth": bytes([2, 0]) + resp("status=badauth\n", "403 Forbidden"),
        "download": bytes([0, 1]) + b"HTTP/1.0 200 OK\r\ncontent-length: 4\r\n\r\nRFUP",
        "url_ipv6": bytes([0, 0]) + b"http://[fd00::45]:8074/site",
    }


# ---- settings and typed text ----

def settings_seeds():
    settings = {"community": "public", "dhcpEnabled": False, "staticIP": "192.168.1.100",
                "subnetMask": "255.255.255.0", "gateway": "192.168.1.1", "ipv6Address": "fd00::5",
                "ipv6Gateway": "fd00::1", "sysName": "rack-4", "sysContact": "ops", "sysLocation": "Rack 4",
                "trapTarget": "10.132.0.45", "trapVersion": 2, "trapFromDhcp": True, "webEnabled": True,
                "webPassword": "0" * 32 + "$" + "a" * 64, "version": 1, "sequence": 42}
    update = {"version": 1, "sequence": 3, "url": "http://10.132.0.45:8074", "fromDhcp": False, "key": "ab" * 32}
    log = {"version": 1, "sequence": 5, "ntpMode": "typed", "ntp": "pool.ntp.org", "syslogMode": "dhcp",
           "syslog": ""}
    return {
        "settings": b"\x00" + json.dumps(settings).encode(),
        "settings_old": b"\x00" + json.dumps({k: settings[k] for k in ("community", "dhcpEnabled", "staticIP",
                                                                      "subnetMask", "gateway", "version")}).encode(),
        "update": b"\x01" + json.dumps(update).encode(),
        "log": b"\x02" + json.dumps(log).encode(),
    }


def text_seeds():
    return {
        "ipv4": b"10.0.0.5\n255.255.255.0\n10.0.0.1",
        "ipv6": b"fd16:d833:4f0e:3ece::249\nfd16:d833:4f0e:3ece::1",
        "ipv6_full": b"2001:0db8:0000:0000:0000:ff00:0042:8329",
        "names": b"pool.ntp.org\n10.132.0.45:514",
        "hostname": b"Rack 4 / UPS -- main",
        "dhcp_none": b"dhcp\nnone",
    }


def cli_seeds():
    """Typed bytes for fuzz_cli_input.cpp: 0xFF toggles secret echo"""
    return {
        "menu": b"1\r\n3\r10.0.0.5\r255.255.255.0\r10.0.0.1\ry\r",
        "snmp": b"4\rrfit\rrack-4\r-\rRack 4\rDHCP\r2c\rn\r",
        "edit": b"abc\b\x7fd\x1b[A\r\n\n",
        "ipv6": b"9\rFD00:0000::0010\r-\r",
        "update": b"10\rhttp://[fd00::45]:8074/site\r\xff" + b"ab" * 32 + b"\r\xff",
        "password": b"6\r\xffsecret12\rsecret12\r\xff",
        "overflow": b"x" * 150 + b"\r1\r",
    }


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    seeds = {
        "snmp_ber": snmp_seeds(),
        "snmp_agent": {k: b"\x07" + v for k, v in snmp_seeds().items()},
        "http_request": {k: pieces(v, 255) for k, v in http_requests().items()}
        | {k + "_small": pieces(v, 7) for k, v in http_requests().items()},
        "web_app": web_seeds(),
        "fw_update": fw_seeds(),
        "dhcp": dhcp_seeds(),
        "dhcp6": dhcp6_seeds(),
        "lldp": lldp_seeds(),
        "update_client": update_client_seeds(),
        "settings": settings_seeds(),
        "text_input": text_seeds(),
        "cli_input": cli_seeds(),
    }
    for harness, files in seeds.items():
        d = os.path.join(out, harness)
        os.makedirs(d, exist_ok=True)
        for name, data in files.items():
            with open(os.path.join(d, name), "wb") as f:
                f.write(data)
    print("seeds for %d harnesses in %s" % (len(seeds), out))


if __name__ == "__main__":
    main()
