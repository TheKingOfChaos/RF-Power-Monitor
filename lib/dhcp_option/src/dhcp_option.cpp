#include "dhcp_option.h"

#include <string.h>

static const size_t ETH_HEADER = 14;
static const size_t BOOTP_FIXED = 236;  // op .. file, then the 4-byte magic cookie
static const uint8_t COOKIE[4] = {99, 130, 83, 99};
static const uint8_t OPT_PAD = 0, OPT_END = 255, OPT_MSG_TYPE = 53, OPT_PARAM_LIST = 55;
static const uint8_t DISCOVER = 1, REQUEST = 3, ACK = 5, INFORM = 8;

static uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

/// One's-complement sum of 16-bit words (an odd last byte is padded with zero)
static uint32_t sum16(const uint8_t* p, size_t n, uint32_t acc) {
    for (size_t i = 0; i + 1 < n; i += 2) acc += be16(p + i);
    if (n & 1) acc += (uint32_t)p[n - 1] << 8;
    return acc;
}

static uint16_t fold(uint32_t acc) {
    while (acc >> 16) acc = (acc & 0xFFFF) + (acc >> 16);
    return (uint16_t)~acc;
}

/// The UDP datagram of an unfragmented IPv4/UDP frame, or false
struct Udp {
    const uint8_t* ip;
    uint8_t* payload;  // writable only for the editing call
    size_t ipHeader, length;  // UDP length incl. its 8-byte header
    uint16_t src, dst;
};

static bool findUdp(const uint8_t* f, size_t len, Udp& u) {
    if (len < ETH_HEADER + 20 + 8) return false;
    if (be16(f + 12) != 0x0800) return false;
    const uint8_t* ip = f + ETH_HEADER;
    if ((ip[0] >> 4) != 4) return false;
    size_t ihl = (size_t)(ip[0] & 15) * 4;
    size_t total = be16(ip + 2);
    if (ihl < 20 || total < ihl + 8 || total > len - ETH_HEADER) return false;
    if (ip[9] != 17) return false;
    if (be16(ip + 6) & 0x3FFF) return false;  // MF flag or fragment offset
    if (fold(sum16(ip, ihl, 0)) != 0) return false;  // header checksum must verify
    const uint8_t* udp = ip + ihl;
    size_t ulen = be16(udp + 4);
    if (ulen < 8 || ulen > total - ihl) return false;
    u.ip = ip;
    u.payload = const_cast<uint8_t*>(udp);
    u.ipHeader = ihl;
    u.length = ulen;
    u.src = be16(udp);
    u.dst = be16(udp + 2);
    return true;
}

/// UDP checksum over the pseudo header and the datagram, with the checksum field read as is
static uint16_t udpChecksum(const Udp& u) {
    uint32_t acc = sum16(u.ip + 12, 8, 0);  // source and destination address
    acc += 17 + (uint32_t)u.length;
    return fold(sum16(u.payload, u.length, acc));
}

/// Where the options are and how far they may go; false if not a BOOTP message of this kind
static bool findOptions(const Udp& u, uint8_t op, size_t& start, size_t& end) {
    const uint8_t* b = u.payload + 8;
    size_t blen = u.length - 8;
    if (blen < BOOTP_FIXED + 4 + 1) return false;
    if (b[0] != op || b[1] != 1 || b[2] != 6) return false;  // Ethernet, 6-byte addresses
    if (memcmp(b + BOOTP_FIXED, COOKIE, 4) != 0) return false;
    start = 8 + BOOTP_FIXED + 4;
    end = u.length;
    return true;
}

/// Walk the options: positions of message type, a code, and END. false if malformed.
struct Walk {
    size_t msgType = 0, param = 0, wanted = 0, serverId = 0, end = 0;
};

static bool walkOptions(const uint8_t* d, size_t start, size_t stop, uint8_t wantedCode, Walk& w) {
    size_t i = start;
    while (i < stop) {
        uint8_t code = d[i];
        if (code == OPT_PAD) {
            i++;
            continue;
        }
        if (code == OPT_END) {
            w.end = i;
            return true;
        }
        if (i + 1 >= stop || i + 2 + d[i + 1] > stop) return false;
        if (code == OPT_MSG_TYPE && w.msgType == 0 && d[i + 1] == 1) w.msgType = i;
        if (code == OPT_PARAM_LIST && w.param == 0) w.param = i;
        if (code == wantedCode && w.wanted == 0) w.wanted = i;
        if (code == 54 && w.serverId == 0) w.serverId = i;
        i += 2 + d[i + 1];
    }
    return false;  // no END
}

DhcpEdit dhcpRequestOption(uint8_t* frame, size_t len, uint8_t code) {
    Udp u;
    if (frame == nullptr || !findUdp(frame, len, u) || u.src != 68 || u.dst != 67) return DhcpEdit::NotDhcpRequest;
    size_t start, stop;
    if (!findOptions(u, 1, start, stop)) return DhcpEdit::NotDhcpRequest;
    uint8_t* d = u.payload;
    Walk w;
    if (!walkOptions(d, start, stop, 0, w) || w.msgType == 0 || w.param == 0) return DhcpEdit::NotDhcpRequest;
    uint8_t type = d[w.msgType + 2];
    if (type != DISCOVER && type != REQUEST && type != INFORM) return DhcpEdit::NotDhcpRequest;
    uint16_t stored = be16(d + 6);
    if (stored != 0 && udpChecksum(u) != 0) return DhcpEdit::NotDhcpRequest;  // already corrupt: leave it

    uint8_t listLen = d[w.param + 1];
    for (size_t i = 0; i < listLen; i++) {
        if (d[w.param + 2 + i] == code) return DhcpEdit::AlreadyRequested;
    }
    if (listLen == 255 || w.end + 1 >= stop) return DhcpEdit::NoRoom;

    // Shift everything after the list up to and including END by one, into the first padding byte
    size_t insert = w.param + 2 + listLen;
    memmove(d + insert + 1, d + insert, w.end + 1 - insert);
    d[insert] = code;
    d[w.param + 1] = (uint8_t)(listLen + 1);

    if (stored != 0) {
        d[6] = d[7] = 0;
        uint16_t sum = udpChecksum(u);
        if (sum == 0) sum = 0xFFFF;  // 0 means "no checksum" in UDP over IPv4
        d[6] = (uint8_t)(sum >> 8);
        d[7] = (uint8_t)sum;
    }
    return DhcpEdit::Added;
}

DhcpAck dhcpReadAck(const uint8_t* frame, size_t len, const uint8_t mac[6], uint8_t code, DhcpAckInfo& info) {
    Udp u;
    if (frame == nullptr || !findUdp(frame, len, u) || u.src != 67 || u.dst != 68) return DhcpAck::NotDhcpReply;
    if (be16(u.payload + 6) != 0 && udpChecksum(u) != 0) return DhcpAck::NotDhcpReply;
    size_t start, stop;
    if (!findOptions(u, 2, start, stop)) return DhcpAck::NotDhcpReply;
    const uint8_t* d = u.payload;
    const uint8_t* bootp = d + 8;
    if (memcmp(bootp + 28, mac, 6) != 0) return DhcpAck::NotForUs;
    Walk w;
    if (!walkOptions(d, start, stop, code, w) || w.msgType == 0) return DhcpAck::NotDhcpReply;
    if (d[w.msgType + 2] != ACK) return DhcpAck::NotAck;
    memcpy(info.yiaddr, bootp + 16, 4);
    info.xid = (uint32_t)bootp[4] << 24 | (uint32_t)bootp[5] << 16 | (uint32_t)bootp[6] << 8 | bootp[7];
    info.hasServerId = w.serverId != 0 && d[w.serverId + 1] == 4;
    if (info.hasServerId) memcpy(info.serverId, d + w.serverId + 2, 4);
    if (w.wanted == 0 || d[w.wanted + 1] != 4) return DhcpAck::NoOption;
    memcpy(info.value, d + w.wanted + 2, 4);
    return DhcpAck::Option;
}

bool dhcpAckText(const uint8_t* frame, size_t len, uint8_t code, char* out, size_t outSize) {
    if (out == nullptr || outSize == 0) return false;
    out[0] = '\0';
    Udp u;
    if (frame == nullptr || !findUdp(frame, len, u) || u.src != 67 || u.dst != 68) return false;
    if (be16(u.payload + 6) != 0 && udpChecksum(u) != 0) return false;
    size_t start, stop;
    if (!findOptions(u, 2, start, stop)) return false;
    const uint8_t* d = u.payload;
    Walk w;
    if (!walkOptions(d, start, stop, code, w) || w.msgType == 0 || d[w.msgType + 2] != ACK) return false;
    if (w.wanted == 0) return false;
    size_t n = d[w.wanted + 1];
    if (n == 0 || n >= outSize) return false;
    const uint8_t* v = d + w.wanted + 2;
    for (size_t i = 0; i < n; i++) {
        if (v[i] < 0x20 || v[i] > 0x7E) return false;
    }
    memcpy(out, v, n);
    out[n] = '\0';
    return true;
}
