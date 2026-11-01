#include "lldp.h"

#include <string.h>

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
