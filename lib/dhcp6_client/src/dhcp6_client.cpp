#include "dhcp6_client.h"

#include <string.h>

// RFC 8415 section 7.6, in milliseconds
static const uint64_t SOL_MAX_DELAY = 1000;
static const uint64_t SOL_TIMEOUT = 1000;
static const uint64_t SOL_MAX_RT = 3600000;
static const uint64_t REQ_TIMEOUT = 1000;
static const uint64_t REQ_MAX_RT = 30000;
static const uint32_t REQ_MAX_RC = 10;
static const uint64_t REN_TIMEOUT = 10000;
static const uint64_t REN_MAX_RT = 600000;
static const uint64_t REB_TIMEOUT = 10000;
static const uint64_t REB_MAX_RT = 600000;
static const uint64_t DEC_TIMEOUT = 1000;
static const uint32_t DEC_MAX_RC = 4;

static const uint32_t INFINITY_S = 0xFFFFFFFFu;
static const uint64_t NEVER = UINT64_MAX;

const char* dhcp6StateName(Dhcp6State state) {
    switch (state) {
    case Dhcp6State::Idle:       return "idle";
    case Dhcp6State::Soliciting: return "soliciting";
    case Dhcp6State::Requesting: return "requesting";
    case Dhcp6State::Bound:      return "bound";
    case Dhcp6State::Renewing:   return "renewing";
    case Dhcp6State::Rebinding:  return "rebinding";
    case Dhcp6State::Declining:  return "declining";
    }
    return "?";
}

Dhcp6Client::Dhcp6Client(const uint8_t mac[6], uint32_t (*random32)()) : _random32(random32) {
    memcpy(_mac, mac, 6);
    _hostname[0] = '\0';
    memset(&_lease, 0, sizeof(_lease));
    memset(&_best, 0, sizeof(_best));
}

void Dhcp6Client::setHostname(const char* name) {
    size_t n = name ? strlen(name) : 0;
    if (n >= sizeof(_hostname)) n = 0;  // dhcp6Build would refuse it anyway
    if (n > 0) memcpy(_hostname, name, n);
    _hostname[n] = '\0';
}

uint64_t Dhcp6Client::clock(uint32_t nowMs) {
    if (!_clockStarted) {
        _clockStarted = true;
        _last32 = nowMs;
    }
    // millis() wraps every 49 days: add the difference, never step back for an older value
    int32_t step = (int32_t)(nowMs - _last32);
    if (step > 0) {
        _now += (uint32_t)step;
        _last32 = nowMs;
    }
    return _now;
}

/// base + RAND * base, RAND uniform in [-0.1, 0.1] (or (0, 0.1] for the first Solicit)
uint64_t Dhcp6Client::jitter(uint64_t base, bool positiveOnly) {
    if (positiveOnly) return base + base * (_random32() % 1000 + 1) / 10000;
    int64_t r = (int64_t)(_random32() % 2001) - 1000;
    return (uint64_t)((int64_t)base + (int64_t)base * r / 10000);
}

/// RFC 8415 section 15: double, then cap at the state's maximum
uint64_t Dhcp6Client::nextTimeout(uint64_t prev) {
    uint64_t maxRt = _state == Dhcp6State::Soliciting  ? SOL_MAX_RT
                     : _state == Dhcp6State::Requesting ? REQ_MAX_RT
                     : _state == Dhcp6State::Renewing   ? REN_MAX_RT
                     : _state == Dhcp6State::Rebinding  ? REB_MAX_RT
                                                        : 0;
    uint64_t rt = jitter(2 * prev, false);
    if (maxRt != 0 && rt > maxRt) rt = jitter(maxRt, false);
    return rt;
}

void Dhcp6Client::enter(Dhcp6State s, uint64_t firstTxAt) {
    _state = s;
    _xid = _random32() & 0xFFFFFF;
    _nextTx = firstTxAt;
    _count = 0;
    _exchangeSent = false;
}

void Dhcp6Client::restartSolicit() {
    _haveAdvert = false;
    enter(Dhcp6State::Soliciting, _now + _random32() % SOL_MAX_DELAY);
}

void Dhcp6Client::start(uint32_t nowMs) {
    if (_state != Dhcp6State::Idle) return;
    clock(nowMs);
    restartSolicit();
}

/// A lease the client may use: an address, success, and T1 <= T2 when both are set (RFC 8415 21.4)
static bool usable(const Dhcp6In& m) {
    if (!m.hasLease || m.status != DHCP6_STATUS_SUCCESS || m.iaStatus != DHCP6_STATUS_SUCCESS) return false;
    return !(m.lease.t1 > 0 && m.lease.t2 > 0 && m.lease.t1 > m.lease.t2);
}

static uint64_t atSeconds(uint64_t now, uint32_t s) {
    return s == INFINITY_S ? NEVER : now + (uint64_t)s * 1000;
}

void Dhcp6Client::bind(const Dhcp6In& m) {
    if (_haveLease && memcmp(_lease.addr, m.lease.addr, 16) != 0) {
        _pendingRemove = true;  // the server moved us to another address
        memcpy(_removeAddr, _lease.addr, 16);
    }
    memcpy(_serverId, m.serverId, m.serverIdLen);
    _serverIdLen = m.serverIdLen;
    _lease = m.lease;
    _haveLease = true;
    _pendingAdd = true;  // add, or refresh the lifetimes of the address lwIP already has
    _boundAt = _now;

    // T1/T2 0 leave the times to us; a deprecated address (preferred 0) uses the valid lifetime,
    // or T1 would be now and the client would renew once per round trip
    uint32_t base = _lease.preferred ? _lease.preferred : _lease.valid;
    uint32_t t1 = _lease.t1 ? _lease.t1 : (base == INFINITY_S ? INFINITY_S : base / 2);
    uint32_t t2 = _lease.t2 ? _lease.t2 : (base == INFINITY_S ? INFINITY_S : (uint32_t)((uint64_t)base * 4 / 5));
    _t1At = atSeconds(_now, t1);
    _t2At = atSeconds(_now, t2);
    _validEnd = atSeconds(_now, _lease.valid);
    _state = Dhcp6State::Bound;
    _count = 0;
}

void Dhcp6Client::received(const Dhcp6In& m, uint32_t nowMs) {
    clock(nowMs);
    if (_state == Dhcp6State::Idle || _state == Dhcp6State::Bound || m.xid != _xid) {
        _ignored++;
        return;
    }
    bool sameServer = m.serverIdLen == _serverIdLen && memcmp(m.serverId, _serverId, _serverIdLen) == 0;

    switch (_state) {
    case Dhcp6State::Soliciting:
        if (m.type != DHCP6_ADVERTISE || !usable(m)) {
            _ignored++;
            return;
        }
        if (!_haveAdvert || m.preference > _best.preference) _best = m;
        _haveAdvert = true;
        _accepted++;
        // 255 ends the collection at once; after the first RT the first good Advertise is taken
        if (m.preference == 255 || (_count > 0 && _now >= _collectUntil)) {
            memcpy(_serverId, _best.serverId, _best.serverIdLen);
            _serverIdLen = _best.serverIdLen;
            _lease = _best.lease;
            enter(Dhcp6State::Requesting, _now);
        }
        return;

    case Dhcp6State::Requesting:
        if (m.type != DHCP6_REPLY || !sameServer) break;
        _accepted++;
        if (usable(m)) {
            bind(m);
        } else {
            restartSolicit();  // NoAddrsAvail, NotOnLink or no usable address: look again
        }
        return;

    case Dhcp6State::Renewing:
    case Dhcp6State::Rebinding:
        if (m.type != DHCP6_REPLY || (_state == Dhcp6State::Renewing && !sameServer)) break;
        if (usable(m)) {
            _accepted++;
            bind(m);
        } else if (m.iaStatus == DHCP6_STATUS_NOBINDING) {
            _accepted++;  // the server lost our binding: ask for the address again
            memcpy(_serverId, m.serverId, m.serverIdLen);
            _serverIdLen = m.serverIdLen;
            enter(Dhcp6State::Requesting, _now);
        } else {
            break;  // keep trying until T2 / the end of the valid lifetime
        }
        return;

    case Dhcp6State::Declining:
        if (m.type != DHCP6_REPLY) break;
        _accepted++;
        restartSolicit();
        return;

    default:
        break;
    }
    _ignored++;
}

void Dhcp6Client::addressDuplicate(uint32_t nowMs) {
    clock(nowMs);
    if (!_haveLease) return;
    _haveLease = false;
    _pendingAdd = false;
    _pendingRemove = true;
    memcpy(_removeAddr, _lease.addr, 16);
    enter(Dhcp6State::Declining, _now);
}

Dhcp6Action Dhcp6Client::poll(uint32_t nowMs, uint8_t* buf, size_t size) {
    uint64_t t = clock(nowMs);
    Dhcp6Action a;
    memset(&a, 0, sizeof(a));

    if (_pendingRemove) {
        _pendingRemove = false;
        a.kind = Dhcp6Action::RemoveAddress;
        memcpy(a.addr, _removeAddr, 16);
        return a;
    }
    if (_pendingAdd) {
        _pendingAdd = false;
        a.kind = Dhcp6Action::AddAddress;
        memcpy(a.addr, _lease.addr, 16);
        a.preferred = _lease.preferred;
        a.valid = _lease.valid;
        return a;
    }

    // Timers of the lease; the address also expires while the client is asking for it again
    // (Requesting after NoBinding, or Soliciting after that failed)
    bool leased = _state == Dhcp6State::Bound || _state == Dhcp6State::Renewing || _state == Dhcp6State::Rebinding;
    if (_haveLease && t >= _validEnd) {
        _haveLease = false;
        if (leased) restartSolicit();
        a.kind = Dhcp6Action::RemoveAddress;
        memcpy(a.addr, _lease.addr, 16);
        return a;
    }
    if (_state == Dhcp6State::Bound && t >= _t1At) enter(Dhcp6State::Renewing, t);
    if (_state == Dhcp6State::Renewing && t >= _t2At) enter(Dhcp6State::Rebinding, t);
    if (_state == Dhcp6State::Soliciting && _haveAdvert && _count > 0 && t >= _collectUntil) {
        memcpy(_serverId, _best.serverId, _best.serverIdLen);
        _serverIdLen = _best.serverIdLen;
        _lease = _best.lease;
        enter(Dhcp6State::Requesting, t);
    }

    if (_state == Dhcp6State::Idle || _state == Dhcp6State::Bound || t < _nextTx) return a;
    if ((_state == Dhcp6State::Requesting && _count >= REQ_MAX_RC) ||
        (_state == Dhcp6State::Declining && _count >= DEC_MAX_RC)) {
        restartSolicit();
        if (t < _nextTx) return a;
    }

    Dhcp6Out o;
    memset(&o, 0, sizeof(o));
    o.xid = _xid;
    o.hostname = _hostname;
    o.elapsedCs = 0;
    if (_count > 0) {
        uint64_t cs = (t - _exchangeStart) / 10;
        o.elapsedCs = cs > 0xFFFF ? 0xFFFF : (uint16_t)cs;
    }
    switch (_state) {
    case Dhcp6State::Soliciting: o.type = DHCP6_SOLICIT; break;
    case Dhcp6State::Requesting: o.type = DHCP6_REQUEST; break;
    case Dhcp6State::Renewing:   o.type = DHCP6_RENEW; break;
    case Dhcp6State::Rebinding:  o.type = DHCP6_REBIND; break;
    case Dhcp6State::Declining:  o.type = DHCP6_DECLINE; break;
    default: return a;
    }
    if (o.type != DHCP6_SOLICIT) {
        o.serverId = _serverId;
        o.serverIdLen = _serverIdLen;
        o.addr = _lease.addr;
    }
    size_t n = dhcp6Build(o, _mac, buf, size);
    if (n == 0) return a;

    if (_count == 0) {
        _exchangeStart = t;
        uint64_t irt = _state == Dhcp6State::Soliciting  ? SOL_TIMEOUT
                       : _state == Dhcp6State::Requesting ? REQ_TIMEOUT
                       : _state == Dhcp6State::Renewing   ? REN_TIMEOUT
                       : _state == Dhcp6State::Rebinding  ? REB_TIMEOUT
                                                          : DEC_TIMEOUT;
        // The first Solicit's RT must be strictly greater than IRT (RFC 8415 18.2.1)
        _rt = jitter(irt, _state == Dhcp6State::Soliciting);
        if (_state == Dhcp6State::Soliciting) _collectUntil = t + _rt;
    } else {
        _rt = nextTimeout(_rt);
    }
    _count++;
    _nextTx = t + _rt;
    _sent++;
    a.kind = Dhcp6Action::Send;
    a.len = n;
    return a;
}

uint32_t Dhcp6Client::validLeftS(uint32_t nowMs) {
    uint64_t t = clock(nowMs);
    if (!_haveLease) return 0;
    if (_validEnd == NEVER) return INFINITY_S;
    return _validEnd > t ? (uint32_t)((_validEnd - t) / 1000) : 0;
}

int dhcp6PickSlot(const uint8_t* states, int count) {
    for (int i = 1; i < count; i++) {
        if (states[i] == 0) return i;
    }
    return -1;
}

bool dhcp6SlotHolds(uint8_t state, const uint8_t slotAddr[16], const uint8_t addr[16]) {
    return state != 0 && memcmp(slotAddr, addr, 16) == 0;
}

// lwIP's IP6_ADDR_DEPRECATED and IP6_ADDR_PREFERRED (the service checks they still match)
static const uint8_t STATE_DEPRECATED = 0x10;
static const uint8_t STATE_PREFERRED = 0x30;

uint8_t dhcp6RefreshedState(uint8_t state, uint32_t preferred) {
    return state == STATE_DEPRECATED && preferred > 0 ? STATE_PREFERRED : state;
}
