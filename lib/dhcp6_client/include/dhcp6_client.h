#pragma once

/**
 * @file dhcp6_client.h
 * @brief Stateful DHCPv6 client for one address (IA_NA), RFC 8415.
 *
 * Driven by the caller: start() when a router advertisement has the M flag,
 * received() for every Advertise/Reply that dhcp6Parse() accepted, addressDuplicate()
 * when duplicate address detection fails, and poll() regularly, which returns what to
 * do next (send a message it wrote into the caller's buffer, add or remove the
 * address) until it returns None. Retransmission follows RFC 8415 section 15 with the
 * constants of section 7.6. Times are wrap-safe: the client extends millis() into a
 * 64-bit clock, so lifetimes up to "infinity" (0xFFFFFFFF s) work. Once started it
 * runs until reboot. Pure C++, host tested.
 */

#include <stddef.h>
#include <stdint.h>

#include "dhcp6_message.h"

/**
 * @brief Where the client is
 */
enum class Dhcp6State : uint8_t {
    Idle,        ///< Not started: no router asked for DHCPv6
    Soliciting,  ///< Looking for a server
    Requesting,  ///< Asking the chosen server for the address
    Bound,       ///< Address leased
    Renewing,    ///< After T1: asking the same server to extend the lease
    Rebinding,   ///< After T2: asking any server to extend the lease
    Declining    ///< The address was a duplicate: telling the server
};

/**
 * @brief Short name for the status screen, e.g. "bound"
 */
const char* dhcp6StateName(Dhcp6State state);

/**
 * @brief What the caller must do next
 */
struct Dhcp6Action {
    enum Kind : uint8_t {
        None,          ///< Nothing until the next poll
        Send,          ///< Send the len bytes written to the poll buffer (a DHCPv6 message)
        AddAddress,    ///< Add addr with these lifetimes (seconds), or refresh them if it is already there
                       ///< (after a Renew/Rebind); a new address goes through duplicate address detection
        RemoveAddress,    ///< Remove addr
        DeprecateAddress  ///< Keep addr only for valid more seconds, no longer preferred (the server moved us)
    } kind;
    size_t len;
    uint8_t addr[16];
    uint32_t preferred;
    uint32_t valid;
};

/**
 * @brief The client
 */
class Dhcp6Client {
public:
    /**
     * @param mac This device's MAC (DUID-LL)
     * @param random32 Random source for transaction ids and retransmission jitter
     */
    Dhcp6Client(const uint8_t mac[6], uint32_t (*random32)());

    /// Name sent in the Client FQDN option (copied; "" for none)
    void setHostname(const char* name);

    /// Begin soliciting (after a random delay up to 1 s); no-op once started
    void start(uint32_t nowMs);

    /// An Advertise or Reply for this client (from dhcp6Parse)
    void received(const Dhcp6In& m, uint32_t nowMs);

    /// Duplicate address detection failed for the leased address
    void addressDuplicate(uint32_t nowMs);

    /**
     * @brief Next action; call until it returns None
     * @param buf Where a message to send is written (256 bytes are enough)
     */
    Dhcp6Action poll(uint32_t nowMs, uint8_t* buf, size_t size);

    Dhcp6State state() const { return _state; }
    bool hasLease() const { return _haveLease; }
    const Dhcp6Lease& lease() const { return _lease; }
    /// Seconds left of the lease's valid lifetime (0 without a lease, 0xFFFFFFFF for infinity)
    uint32_t validLeftS(uint32_t nowMs);
    /// Seconds left of the lease's preferred lifetime (0 without a lease or once deprecated, 0xFFFFFFFF for infinity)
    uint32_t preferredLeftS(uint32_t nowMs);

    uint32_t sent() const { return _sent; }
    uint32_t accepted() const { return _accepted; }
    uint32_t ignored() const { return _ignored; }

private:
    uint64_t clock(uint32_t nowMs);
    void enter(Dhcp6State s, uint64_t firstTxAt);
    void restartSolicit();
    void bind(const Dhcp6In& m);
    uint64_t jitter(uint64_t base, bool positiveOnly);
    uint64_t nextTimeout(uint64_t prev);

    uint8_t _mac[6];
    uint32_t (*_random32)();
    char _hostname[64];

    // 64-bit milliseconds since construction, extended from millis()
    uint64_t _now = 0;
    uint32_t _last32 = 0;
    bool _clockStarted = false;

    Dhcp6State _state = Dhcp6State::Idle;
    uint32_t _xid = 0;
    uint64_t _nextTx = 0;          ///< When the next (re)transmission is due
    uint64_t _rt = 0;              ///< Current retransmission timeout
    uint64_t _exchangeStart = 0;   ///< First transmission of this exchange (Elapsed Time)
    uint32_t _count = 0;           ///< Transmissions in this exchange
    uint64_t _solMaxRt;            ///< Solicit's maximum retransmission timeout (a server may set it)

    // Soliciting: the best Advertise so far
    bool _haveAdvert = false;
    uint64_t _collectUntil = 0;
    Dhcp6In _best;

    // The server and lease
    uint8_t _serverId[DHCP6_SERVER_ID_MAX];
    uint8_t _serverIdLen = 0;
    bool _haveLease = false;
    Dhcp6Lease _lease;
    uint64_t _t1At = 0, _t2At = 0, _validEnd = 0, _preferredEnd = 0;

    // An address the server moved us away from: kept, deprecated, until its valid end
    bool _haveRetired = false;
    bool _pendingRetire = false;
    uint8_t _retiredAddr[16];
    uint64_t _retiredEnd = 0;

    // Address actions waiting for poll()
    bool _pendingRemove = false;
    uint8_t _removeAddr[16];
    bool _pendingAdd = false;

    uint32_t _sent = 0, _accepted = 0, _ignored = 0;
};

/**
 * @brief Choose the lwIP address slot for the DHCPv6 address
 * @param states lwIP address states (0 = invalid, i.e. free), slot 0 is the link-local address
 * @return Highest free slot after 0, -1 if all are used (nothing is ever evicted). Highest because lwIP
 *         gives a router advertisement's lifetimes to the first address in the prefix that is not static,
 *         and SLAAC takes the first free slot
 */
int dhcp6PickSlot(const uint8_t* states, int count);

/**
 * @brief Whether an lwIP slot still holds the DHCPv6 address (lwIP may have expired it, or SLAAC reused it)
 * @param state The slot's lwIP state (0 = invalid)
 * @param slotAddr The address now in the slot
 * @param addr The leased address
 */
bool dhcp6SlotHolds(uint8_t state, const uint8_t slotAddr[16], const uint8_t addr[16]);

/**
 * @brief lwIP state for the preferred lifetime left: a deprecated address with a new preferred lifetime
 * is preferred again (lwIP's timer only ever moves preferred to deprecated), and a preferred one whose
 * lifetime is over is deprecated (a router advertisement for the prefix may have undone that)
 */
uint8_t dhcp6RefreshedState(uint8_t state, uint32_t preferred);
