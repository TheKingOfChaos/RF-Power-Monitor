#pragma once

/**
 * @file lldp.h
 * @brief LLDP (IEEE 802.1AB): the frames the device sends, the switch's frames
 *        it reads, the one neighbour it keeps, and when to send.
 *
 * Pure C++, unit tested on the host. src/lldp_service.cpp runs it: received
 * frames come from the input hook in src/net_stats.cpp, built frames go out
 * with netStatsSendFrame().
 */

#include <stddef.h>
#include <stdint.h>

static const uint16_t LLDP_ETHERTYPE = 0x88CC;
static const uint16_t LLDP_TTL_S = 120;    ///< Time to live we announce: 4 x the send interval
static const size_t LLDP_FRAME_MAX = 320;  ///< Send buffer: our frame with 32-byte names and both addresses fits

/**
 * @brief What the device announces about itself
 */
struct LldpLocal {
    uint8_t mac[6];         ///< Chassis ID and Port ID (MAC subtypes) and the frame's source
    const char* sysName;    ///< Hostname (= sysName)
    const char* sysDescr;   ///< Same text as SNMP sysDescr
    const char* portDescr;  ///< Same text as IF-MIB ifDescr
    bool hasIPv4;
    uint8_t ipv4[4];
    bool hasIPv6;
    uint8_t ipv6[16];       ///< A global address (not link-local)
    uint16_t ttl;           ///< Seconds, normally LLDP_TTL_S
};

/**
 * @brief Build the whole Ethernet frame (header + LLDPDU) to the nearest-bridge
 *        address 01:80:c2:00:00:0e
 *
 * TLVs: Chassis ID (MAC), Port ID (MAC), TTL, Port Description, System Name,
 * System Description, System Capabilities (station only), one Management
 * Address per address family present (ifIndex 1, no OID), End.
 *
 * @return Frame length, or 0 if out is null, cap is too small or a string is
 *         longer than a TLV holds (255 bytes)
 */
size_t lldpBuildFrame(const LldpLocal& local, uint8_t* out, size_t cap);

static const size_t LLDP_TEXT_MAX = 64;  ///< Received text kept per field (bytes, without the NUL)
static const size_t LLDP_ID_MAX = 64;    ///< Raw Chassis ID / Port ID bytes kept
static const size_t LLDP_SYS_DESCR_MAX = 128;  ///< System Description kept (bytes, without the NUL)

/**
 * @brief What a neighbour (the switch) announced, as printable text
 *
 * Text fields hold at most LLDP_TEXT_MAX bytes; bytes outside 0x20..0x7E are
 * replaced by '?'. The web page still has to HTML-escape them.
 */
struct LldpNeighbour {
    char chassisId[LLDP_TEXT_MAX + 1];  ///< MAC as aa:bb:cc:dd:ee:ff, network address as text, else the text
    uint8_t portIdSubtype;              ///< 802.1AB Port ID subtype (7 = locally assigned, Junos ifIndex)
    char portId[LLDP_TEXT_MAX + 1];     ///< Same rules as chassisId
    char portDescr[LLDP_TEXT_MAX + 1];  ///< Empty if not sent
    char sysName[LLDP_TEXT_MAX + 1];    ///< Empty if not sent
    char mgmtAddr[46];                  ///< First IPv4 or IPv6 management address as text, empty if none
    uint16_t ttl;                       ///< Seconds; 0 = the neighbour is shutting down
    // As received, for the LLDP-MIB lldpRemTable (src/snmp_service.cpp)
    uint8_t chassisIdSubtype;
    uint8_t chassisIdRaw[LLDP_ID_MAX];  ///< Chassis ID bytes after the subtype, cut to LLDP_ID_MAX
    uint8_t chassisIdLen;
    uint8_t portIdRaw[LLDP_ID_MAX];     ///< Port ID bytes after the subtype, cut to LLDP_ID_MAX
    uint8_t portIdLen;
    char sysDescr[LLDP_SYS_DESCR_MAX + 1];  ///< System Description, same text rules, empty if not sent
    uint16_t capsSupported;             ///< System Capabilities bits, 0 if not sent
    uint16_t capsEnabled;
};

/**
 * @brief The Port ID as served in LLDP-MIB lldpRemPortId
 *
 * Junos sends its ifIndex as a locally assigned Port ID (subtype 7) and the
 * interface name as Port Description; LibreNMS labels the remote port with
 * lldpRemPortId. So for subtype 7 with a description, this gives subtype 5
 * (interfaceName) and the description, else the Port ID as received.
 *
 * @param data Points into n; valid as long as n is
 */
void lldpSnmpPortId(const LldpNeighbour& n, uint8_t& subtype, const uint8_t*& data, size_t& len);

enum class LldpParse : uint8_t { Ok, NotLldp, Malformed };

/**
 * @brief Read an Ethernet frame carrying an LLDPDU
 *
 * Malformed: a TLV running past the frame, Chassis ID, Port ID and TTL not
 * the first three TLVs, a bad length in one of them, or no End TLV. Unknown
 * and organisational TLVs are skipped.
 *
 * @param out Filled on Ok, unspecified otherwise
 * @return NotLldp for a null, short (< 14 bytes) or non-LLDP frame
 */
LldpParse lldpParseFrame(const uint8_t* frame, size_t len, LldpNeighbour& out);

/**
 * @brief The one neighbour the device keeps: the latest frame wins, and it is
 *        forgotten when its TTL runs out (millis() wrap safe) or on TTL 0
 */
class LldpNeighbourTable {
public:
    /// Take a parsed neighbour received at nowMs; TTL 0 forgets the neighbour
    void received(const LldpNeighbour& n, uint32_t nowMs);
    /// Forget the neighbour once its TTL has run out
    void expire(uint32_t nowMs);
    /// Forget the neighbour (link down)
    void clear();
    bool present() const { return _present; }
    /// Valid while present()
    const LldpNeighbour& neighbour() const { return _n; }

private:
    LldpNeighbour _n = {};
    uint32_t _receivedMs = 0;
    bool _present = false;
};

/**
 * @brief "<name>, port <port>", or "none seen" without a neighbour, cut to cap
 *
 * Name: System Name, else Chassis ID. Port: Port Description when the Port ID
 * is locally assigned (subtype 7, as Junos sends the ifIndex) and a
 * description was sent, else the Port ID.
 */
void lldpSwitchText(const LldpNeighbourTable& table, char* out, size_t cap);

static const uint32_t LLDP_INTERVAL_MS = 30000;  ///< Regular send interval (802.1AB msgTxInterval)
static const uint32_t LLDP_MIN_GAP_MS = 1000;    ///< Wait after link up, and the least time between frames

/**
 * @brief When to send: 1 s after the link comes up, every 30 s, and 1 s after
 *        the last frame when the content changed (address or hostname)
 */
class LldpSchedule {
public:
    /**
     * @brief Ask whether a frame should go out now (call regularly)
     * @param contentChanged The frame differs from the last one sent
     */
    bool due(uint32_t nowMs, bool linkUp, bool contentChanged);
    /// A frame went out at nowMs
    void sent(uint32_t nowMs);

private:
    bool _linkUp = false;
    bool _sentSinceLinkUp = false;
    uint32_t _linkUpMs = 0;
    uint32_t _sentMs = 0;
};
