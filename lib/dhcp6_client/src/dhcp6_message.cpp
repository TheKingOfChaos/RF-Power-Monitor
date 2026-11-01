#include "dhcp6_message.h"

#include <string.h>

// Option codes (RFC 8415 section 21, RFC 4704)
static const uint16_t OPT_CLIENTID = 1;
static const uint16_t OPT_SERVERID = 2;
static const uint16_t OPT_IA_NA = 3;
static const uint16_t OPT_IAADDR = 5;
static const uint16_t OPT_ORO = 6;
static const uint16_t OPT_PREFERENCE = 7;
static const uint16_t OPT_ELAPSED_TIME = 8;
static const uint16_t OPT_STATUS_CODE = 13;
static const uint16_t OPT_CLIENT_FQDN = 39;
static const uint16_t OPT_SOL_MAX_RT = 82;

static const uint8_t FQDN_FLAG_S = 0x01;  ///< Server should update the AAAA record
static const size_t DUID_LL_LEN = 10;     ///< type 3, hardware type 1, MAC
static const size_t LABEL_MAX = 63;

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static void duidLL(const uint8_t mac[6], uint8_t out[DUID_LL_LEN]) {
    out[0] = 0x00;
    out[1] = 0x03;  // DUID-LL
    out[2] = 0x00;
    out[3] = 0x01;  // Ethernet
    memcpy(out + 4, mac, 6);
}

namespace {

/// Appends to a fixed buffer; once anything does not fit, everything after is dropped and ok is false
struct Writer {
    uint8_t* p;
    size_t size;
    size_t n = 0;
    bool ok = true;

    void bytes(const uint8_t* src, size_t len) {
        if (!ok || len > size - n) {
            ok = false;
            return;
        }
        memcpy(p + n, src, len);
        n += len;
    }
    void u8(uint8_t v) { bytes(&v, 1); }
    void u16(uint16_t v) {
        const uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v};
        bytes(b, 2);
    }
    void u32(uint32_t v) {
        const uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
        bytes(b, 4);
    }
    /// Option header with a length filled in by end()
    size_t begin(uint16_t code) {
        u16(code);
        size_t at = n;
        u16(0);
        return at;
    }
    void end(size_t at) {
        if (!ok) return;
        uint16_t len = (uint16_t)(n - at - 2);
        p[at] = (uint8_t)(len >> 8);
        p[at + 1] = (uint8_t)len;
    }
};

}  // namespace

size_t dhcp6Build(const Dhcp6Out& m, const uint8_t mac[6], uint8_t* out, size_t size) {
    size_t hostLen = m.hostname ? strlen(m.hostname) : 0;
    bool fqdn = hostLen > 0 && m.type != DHCP6_DECLINE;
    if (hostLen > LABEL_MAX || (m.hostname && strchr(m.hostname, '.'))) return 0;

    Writer w{out, size};
    w.u8(m.type);
    w.u8((uint8_t)(m.xid >> 16));
    w.u8((uint8_t)(m.xid >> 8));
    w.u8((uint8_t)m.xid);

    uint8_t duid[DUID_LL_LEN];
    duidLL(mac, duid);
    size_t at = w.begin(OPT_CLIENTID);
    w.bytes(duid, sizeof(duid));
    w.end(at);

    bool withServer = m.type == DHCP6_REQUEST || m.type == DHCP6_RENEW || m.type == DHCP6_DECLINE;
    if (withServer && m.serverId != nullptr) {
        at = w.begin(OPT_SERVERID);
        w.bytes(m.serverId, m.serverIdLen);
        w.end(at);
    }

    at = w.begin(OPT_IA_NA);
    w.u32(DHCP6_IAID);
    w.u32(0);  // T1, T2: the server decides
    w.u32(0);
    if (m.addr != nullptr) {
        size_t a = w.begin(OPT_IAADDR);
        w.bytes(m.addr, 16);
        w.u32(0);  // lifetimes: the server decides
        w.u32(0);
        w.end(a);
    }
    w.end(at);

    if (m.type != DHCP6_DECLINE) {
        at = w.begin(OPT_ORO);
        if (fqdn) w.u16(OPT_CLIENT_FQDN);
        w.u16(OPT_SOL_MAX_RT);
        w.end(at);
    }

    at = w.begin(OPT_ELAPSED_TIME);
    w.u16(m.elapsedCs);
    w.end(at);

    if (fqdn) {
        // A partial name: one label, no terminating zero-length label (RFC 4704 section 4.2)
        at = w.begin(OPT_CLIENT_FQDN);
        w.u8(FQDN_FLAG_S);
        w.u8((uint8_t)hostLen);
        w.bytes((const uint8_t*)m.hostname, hostLen);
        w.end(at);
    }
    return w.ok ? w.n : 0;
}

/**
 * @brief Read our IA_NA's options: the first usable address (preferring one not deprecated) and the IA's status
 * @return false if the sub-options are malformed
 */
static bool readIaNa(const uint8_t* p, size_t len, Dhcp6In& in) {
    in.lease.t1 = rd32(p + 4);
    in.lease.t2 = rd32(p + 8);
    size_t i = 12;
    while (i < len) {
        if (len - i < 4) return false;
        uint16_t code = rd16(p + i);
        uint16_t olen = rd16(p + i + 2);
        if (olen > len - i - 4) return false;
        const uint8_t* v = p + i + 4;
        if (code == OPT_IAADDR) {
            if (olen < 24) return false;
            uint32_t preferred = rd32(v + 16), valid = rd32(v + 20);
            // The first usable address, but one still preferred beats a deprecated one (renumbering)
            bool better = !in.hasLease || (in.lease.preferred == 0 && preferred > 0);
            if (better && valid != 0 && preferred <= valid) {
                memcpy(in.lease.addr, v, 16);
                in.lease.preferred = preferred;
                in.lease.valid = valid;
                in.hasLease = true;
            }
        } else if (code == OPT_STATUS_CODE) {
            if (olen < 2) return false;
            in.iaStatus = rd16(v);
        }
        i += 4 + olen;
    }
    return true;
}

Dhcp6Parse dhcp6Parse(const uint8_t* msg, size_t len, const uint8_t mac[6], Dhcp6In& out) {
    if (msg == nullptr || len < 4) return Dhcp6Parse::Malformed;
    if (msg[0] != DHCP6_ADVERTISE && msg[0] != DHCP6_REPLY) return Dhcp6Parse::NotServerMessage;

    // Built in a static copy (about 190 bytes): out is written only on success, and core 0's stack is small
    static Dhcp6In in;
    memset(&in, 0, sizeof(in));
    in.type = msg[0];
    in.xid = (uint32_t)msg[1] << 16 | (uint32_t)msg[2] << 8 | msg[3];

    uint8_t duid[DUID_LL_LEN];
    duidLL(mac, duid);
    bool haveClient = false, ours = false;
    size_t i = 4;
    while (i < len) {
        if (len - i < 4) return Dhcp6Parse::Malformed;
        uint16_t code = rd16(msg + i);
        uint16_t olen = rd16(msg + i + 2);
        if (olen > len - i - 4) return Dhcp6Parse::Malformed;
        const uint8_t* v = msg + i + 4;
        switch (code) {
        case OPT_CLIENTID:
            haveClient = true;
            ours = olen == DUID_LL_LEN && memcmp(v, duid, DUID_LL_LEN) == 0;
            break;
        case OPT_SERVERID:
            if (olen == 0 || olen > DHCP6_SERVER_ID_MAX) return Dhcp6Parse::Malformed;
            memcpy(in.serverId, v, olen);
            in.serverIdLen = (uint8_t)olen;
            break;
        case OPT_PREFERENCE:
            if (olen != 1) return Dhcp6Parse::Malformed;
            in.preference = v[0];
            break;
        case OPT_STATUS_CODE:
            if (olen < 2) return Dhcp6Parse::Malformed;
            in.status = rd16(v);
            break;
        case OPT_IA_NA:
            if (olen < 12) return Dhcp6Parse::Malformed;
            if (rd32(v) == DHCP6_IAID && !readIaNa(v, olen, in)) return Dhcp6Parse::Malformed;
            break;
        default:
            break;  // options this client does not use
        }
        i += 4 + olen;
    }
    if (!haveClient || in.serverIdLen == 0) return Dhcp6Parse::Malformed;
    if (!ours) return Dhcp6Parse::NotForUs;
    out = in;
    return Dhcp6Parse::Ok;
}
