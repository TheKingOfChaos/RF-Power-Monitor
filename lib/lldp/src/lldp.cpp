#include "lldp.h"

#include <stdio.h>
#include <string.h>

#include "netconfig.h"

// TLV types (IEEE 802.1AB-2016 clause 8.4 and 8.5)
static const uint8_t TLV_END = 0;
static const uint8_t TLV_CHASSIS_ID = 1;
static const uint8_t TLV_PORT_ID = 2;
static const uint8_t TLV_TTL = 3;
static const uint8_t TLV_PORT_DESCR = 4;
static const uint8_t TLV_SYS_NAME = 5;
static const uint8_t TLV_SYS_DESCR = 6;
static const uint8_t TLV_SYS_CAPS = 7;
static const uint8_t TLV_MGMT_ADDR = 8;

static const uint8_t CHASSIS_MAC = 4;
static const uint8_t PORT_MAC = 3;
static const uint16_t CAP_STATION = 0x0080;
static const uint8_t IANA_IPV4 = 1;
static const uint8_t IANA_IPV6 = 2;
static const uint8_t IF_SUBTYPE_IFINDEX = 2;
static const uint32_t OUR_IFINDEX = 1;

static const uint8_t NEAREST_BRIDGE[6] = {0x01, 0x80, 0xC2, 0x00, 0x00, 0x0E};

/// Appends TLVs to a buffer; any write past cap marks it failed
class TlvWriter {
public:
    TlvWriter(uint8_t* out, size_t cap) : _out(out), _cap(cap) {}

    void raw(const void* data, size_t len) {
        if (_failed || len > _cap - _len) {
            _failed = true;
            return;
        }
        memcpy(_out + _len, data, len);
        _len += len;
    }
    void byte(uint8_t b) { raw(&b, 1); }
    void u16(uint16_t v) {
        uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v};
        raw(b, 2);
    }
    void u32(uint32_t v) {
        uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
        raw(b, 4);
    }
    /// TLV header: 7-bit type, 9-bit length
    void header(uint8_t type, size_t len) {
        if (len > 511) _failed = true;
        u16((uint16_t)((type << 9) | (len & 0x1FF)));
    }
    /// TLV with a one-byte subtype followed by data
    void subtyped(uint8_t type, uint8_t subtype, const uint8_t* data, size_t len) {
        header(type, 1 + len);
        byte(subtype);
        raw(data, len);
    }
    /// Text TLV; LLDP strings hold at most 255 bytes
    void text(uint8_t type, const char* s) {
        size_t len = s ? strlen(s) : 0;
        if (len > 255) _failed = true;
        header(type, len);
        raw(s, len);
    }
    void mgmtAddress(uint8_t family, const uint8_t* addr, size_t addrLen) {
        header(TLV_MGMT_ADDR, 1 + 1 + addrLen + 1 + 4 + 1);
        byte((uint8_t)(1 + addrLen));  // address string length: subtype + address
        byte(family);
        raw(addr, addrLen);
        byte(IF_SUBTYPE_IFINDEX);
        u32(OUR_IFINDEX);
        byte(0);  // no OID
    }

    size_t length() const { return _failed ? 0 : _len; }

private:
    uint8_t* _out;
    size_t _cap;
    size_t _len = 0;
    bool _failed = false;
};

size_t lldpBuildFrame(const LldpLocal& local, uint8_t* out, size_t cap) {
    if (out == nullptr) return 0;
    TlvWriter w(out, cap);
    w.raw(NEAREST_BRIDGE, 6);
    w.raw(local.mac, 6);
    w.u16(LLDP_ETHERTYPE);
    w.subtyped(TLV_CHASSIS_ID, CHASSIS_MAC, local.mac, 6);
    w.subtyped(TLV_PORT_ID, PORT_MAC, local.mac, 6);
    w.header(TLV_TTL, 2);
    w.u16(local.ttl);
    w.text(TLV_PORT_DESCR, local.portDescr);
    w.text(TLV_SYS_NAME, local.sysName);
    w.text(TLV_SYS_DESCR, local.sysDescr);
    w.header(TLV_SYS_CAPS, 4);
    w.u16(CAP_STATION);
    w.u16(CAP_STATION);
    if (local.hasIPv4) w.mgmtAddress(IANA_IPV4, local.ipv4, 4);
    if (local.hasIPv6) w.mgmtAddress(IANA_IPV6, local.ipv6, 16);
    w.header(TLV_END, 0);
    return w.length();
}

// ---- parsing ----

static const uint8_t PORT_LOCAL = 7;
static const uint8_t CHASSIS_NETWORK_ADDRESS = 5;
static const uint8_t PORT_NETWORK_ADDRESS = 4;

/// Copy received text, cut to LLDP_TEXT_MAX, non-printable bytes as '?'
static void copyText(const uint8_t* src, size_t len, char* dest) {
    if (len > LLDP_TEXT_MAX) len = LLDP_TEXT_MAX;
    for (size_t i = 0; i < len; i++) dest[i] = (src[i] >= 0x20 && src[i] <= 0x7E) ? (char)src[i] : '?';
    dest[len] = '\0';
}

/// IANA address family number + address as text; false for other families or lengths
static bool formatAddress(uint8_t family, const uint8_t* addr, size_t len, char* out) {
    if ((family == IANA_IPV4) && len == 4) {
        snprintf(out, 16, "%u.%u.%u.%u", addr[0], addr[1], addr[2], addr[3]);
        return true;
    }
    if ((family == IANA_IPV6) && len == 16) {
        formatIPv6(addr, out);
        return true;
    }
    return false;
}

/// Chassis ID or Port ID value (subtype byte first) as text
static void formatId(const uint8_t* v, size_t len, uint8_t macSubtype, uint8_t addressSubtype, char* dest) {
    const uint8_t* data = v + 1;
    size_t n = len - 1;
    if (v[0] == macSubtype && n == 6) {
        snprintf(dest, LLDP_TEXT_MAX + 1, "%02x:%02x:%02x:%02x:%02x:%02x", data[0], data[1], data[2], data[3],
                 data[4], data[5]);
        return;
    }
    if (v[0] == addressSubtype && n >= 1 && formatAddress(data[0], data + 1, n - 1, dest)) return;
    copyText(data, n, dest);
}

LldpParse lldpParseFrame(const uint8_t* frame, size_t len, LldpNeighbour& out) {
    if (frame == nullptr || len < 14) return LldpParse::NotLldp;
    if (((frame[12] << 8) | frame[13]) != LLDP_ETHERTYPE) return LldpParse::NotLldp;
    memset(&out, 0, sizeof(out));
    size_t at = 14;
    int index = 0;
    while (true) {
        if (len - at < 2) return LldpParse::Malformed;  // the TLVs ran out without an End
        uint8_t type = frame[at] >> 1;
        size_t tlvLen = ((frame[at] & 0x01) << 8) | frame[at + 1];
        at += 2;
        if (tlvLen > len - at) return LldpParse::Malformed;
        const uint8_t* v = frame + at;
        at += tlvLen;
        // Chassis ID, Port ID and TTL come first, in this order (802.1AB 8.2)
        if (index < 3 && type != TLV_CHASSIS_ID + index) return LldpParse::Malformed;
        index++;
        switch (type) {
        case TLV_END:
            return LldpParse::Ok;
        case TLV_CHASSIS_ID:
            if (index != 1 || tlvLen < 2 || tlvLen > 256) return LldpParse::Malformed;
            formatId(v, tlvLen, CHASSIS_MAC, CHASSIS_NETWORK_ADDRESS, out.chassisId);
            break;
        case TLV_PORT_ID:
            if (index != 2 || tlvLen < 2 || tlvLen > 256) return LldpParse::Malformed;
            out.portIdSubtype = v[0];
            formatId(v, tlvLen, PORT_MAC, PORT_NETWORK_ADDRESS, out.portId);
            break;
        case TLV_TTL:
            if (index != 3 || tlvLen != 2) return LldpParse::Malformed;
            out.ttl = (uint16_t)((v[0] << 8) | v[1]);
            break;
        case TLV_PORT_DESCR:
            copyText(v, tlvLen, out.portDescr);
            break;
        case TLV_SYS_NAME:
            copyText(v, tlvLen, out.sysName);
            break;
        case TLV_MGMT_ADDR:
            // Address string length (subtype + address), subtype, address; the rest is not used
            if (out.mgmtAddr[0] == '\0' && tlvLen >= 2 && v[0] >= 2 && (size_t)v[0] + 1 <= tlvLen) {
                if (!formatAddress(v[1], v + 2, v[0] - 1, out.mgmtAddr)) out.mgmtAddr[0] = '\0';
            }
            break;
        default:
            break;  // System Description, Capabilities, organisational TLVs: not shown
        }
    }
}

// ---- the neighbour ----

void LldpNeighbourTable::received(const LldpNeighbour& n, uint32_t nowMs) {
    if (n.ttl == 0) {
        clear();
        return;
    }
    _n = n;
    _receivedMs = nowMs;
    _present = true;
}

void LldpNeighbourTable::expire(uint32_t nowMs) {
    if (_present && nowMs - _receivedMs >= (uint32_t)_n.ttl * 1000u) clear();
}

void LldpNeighbourTable::clear() {
    _present = false;
}

void lldpSwitchText(const LldpNeighbourTable& table, char* out, size_t cap) {
    if (out == nullptr || cap == 0) return;
    if (!table.present()) {
        snprintf(out, cap, "none seen");
        return;
    }
    const LldpNeighbour& n = table.neighbour();
    const char* name = n.sysName[0] ? n.sysName : n.chassisId;
    const char* port = (n.portIdSubtype == PORT_LOCAL && n.portDescr[0]) ? n.portDescr : n.portId;
    snprintf(out, cap, "%s, port %s", name, port);
}

// ---- when to send ----

bool LldpSchedule::due(uint32_t nowMs, bool linkUp, bool contentChanged) {
    if (!linkUp) {
        _linkUp = false;
        return false;
    }
    if (!_linkUp) {
        _linkUp = true;
        _linkUpMs = nowMs;
        _sentSinceLinkUp = false;
    }
    if (!_sentSinceLinkUp) return nowMs - _linkUpMs >= LLDP_MIN_GAP_MS;
    uint32_t since = nowMs - _sentMs;
    return since >= LLDP_INTERVAL_MS || (contentChanged && since >= LLDP_MIN_GAP_MS);
}

void LldpSchedule::sent(uint32_t nowMs) {
    _sentMs = nowMs;
    _sentSinceLinkUp = true;
}
