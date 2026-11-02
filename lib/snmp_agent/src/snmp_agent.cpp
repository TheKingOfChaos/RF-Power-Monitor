#include "snmp_agent.h"

#include <string.h>

bool SnmpAgent::communityMatches(const SnmpRequest& req) const {
    if (_community == nullptr) return false;
    size_t len = strlen(_community);
    if (req.communityLength != len) return false;
    // Compare every byte so timing does not reveal how much matched
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) diff |= (uint8_t)(req.community[i] ^ (uint8_t)_community[i]);
    return diff == 0;
}

size_t SnmpAgent::respondError(const SnmpRequest& req, SnmpError error, uint8_t index, uint8_t* out,
                               size_t cap) {
    // v1 echoes the request varbinds on every error; a v2c tooBig has an empty list
    bool v2c = req.version == SNMP_VERSION_2C;
    size_t len = snmpBuildErrorResponse(req, error, index, out, cap, !(v2c && error == SnmpError::TooBig));
    if (len == 0 && error != SnmpError::TooBig) {
        // The request itself does not fit in the response buffer
        error = SnmpError::TooBig;
        len = snmpBuildErrorResponse(req, error, 0, out, cap, !v2c);
    }
    if (len == 0) {
        _outcome = SnmpOutcome::NoRoom;
        return 0;
    }

    if (error == SnmpError::NoSuchName) _stats.outNoSuchName++;
    if (error == SnmpError::TooBig) _stats.outTooBig++;
    _stats.outResponses++;
    return len;
}

size_t SnmpAgent::finishResponse(SnmpResponseBuilder& builder, uint8_t* response, size_t capacity) {
    size_t len = builder.finish();
    if (len == 0) return respondError(_req, SnmpError::TooBig, 0, response, capacity);
    _stats.outResponses++;
    return len;
}

size_t SnmpAgent::handle(const uint8_t* request, size_t length, uint8_t* response, size_t capacity,
                         const SnmpDeviceState& state) {
    _stats.inPackets++;
    _outcome = SnmpOutcome::Responded;

    switch (snmpParseRequest(request, length, _req)) {
    case SnmpParseResult::Ok:
        break;
    case SnmpParseResult::Malformed:
        _stats.inParseErrors++;
        _outcome = SnmpOutcome::ParseError;
        return 0;
    case SnmpParseResult::BadVersion:
        _stats.inBadVersions++;
        _outcome = SnmpOutcome::BadVersion;
        return 0;
    case SnmpParseResult::UnsupportedPdu:
    case SnmpParseResult::TooManyVarbinds:
        _stats.inUnsupported++;
        _outcome = SnmpOutcome::Unsupported;
        return 0;
    }

    if (!communityMatches(_req)) {
        _stats.inBadCommunity++;
        _outcome = SnmpOutcome::BadCommunity;
        return 0;
    }

    switch (_req.pduType) {
    case BerTag::GetRequest:     _stats.inGetRequests++; break;
    case BerTag::GetNextRequest: _stats.inGetNextRequests++; break;
    case BerTag::GetBulkRequest: _stats.inGetBulkRequests++; break;
    case BerTag::SetRequest:     _stats.inSetRequests++; break;
    }

    if (_req.version == SNMP_VERSION_2C) {
        _stats.inV2cRequests++;
        return handleV2c(response, capacity, state);
    }
    return handleV1(response, capacity, state);
}

// ---- v1 (RFC 1157) ----

/**
 * @brief Resolve one v1 varbind into _found. Counter64 objects do not exist for v1 (RFC 3584 4.2.2.1):
 *        v1 cannot encode them. Neither do absent instances (noSuchName)
 */
bool SnmpAgent::resolveV1(const Oid& name, bool getNext, const SnmpDeviceState& state) {
    if (getNext) return mibGetNext(name, state, MibView::V1, _found);
    return mibGet(name, state, _found) && _found.value.type != BerTag::Counter64;
}

size_t SnmpAgent::handleV1(uint8_t* response, size_t capacity, const SnmpDeviceState& state) {
    if (_req.pduType == BerTag::SetRequest) {
        _outcome = SnmpOutcome::SetRejected;
        // Nothing is writable: RFC 1157 4.1.5 answers noSuchName for the first varbind
        if (_req.varbindCount == 0) return respondError(_req, SnmpError::NoError, 0, response, capacity);
        return respondError(_req, SnmpError::NoSuchName, 1, response, capacity);
    }

    bool getNext = _req.pduType == BerTag::GetNextRequest;

    // Check every varbind first, so errors are reported before anything is built. The second pass
    // resolves again instead of keeping 16 results (~5 KB): the state is a snapshot, so it finds the same
    for (uint8_t i = 0; i < _req.varbindCount; i++) {
        if (!resolveV1(_req.names[i], getNext, state)) {
            return respondError(_req, SnmpError::NoSuchName, i + 1, response, capacity);
        }
    }

    SnmpResponseBuilder builder(response, capacity, _req);
    for (uint8_t i = 0; i < _req.varbindCount; i++) {
        resolveV1(_req.names[i], getNext, state);
        if (!builder.add(_found.oid, _found.value)) {
            return respondError(_req, SnmpError::TooBig, 0, response, capacity);
        }
    }
    return finishResponse(builder, response, capacity);
}

// ---- v2c (RFC 3416) ----

size_t SnmpAgent::handleV2c(uint8_t* response, size_t capacity, const SnmpDeviceState& state) {
    if (_req.pduType == BerTag::SetRequest) {
        _outcome = SnmpOutcome::SetRejected;
        if (_req.varbindCount == 0) return respondError(_req, SnmpError::NoError, 0, response, capacity);
        SnmpError err = mibHasObject(_req.names[0]) ? SnmpError::NotWritable : SnmpError::NoCreation;
        return respondError(_req, err, 1, response, capacity);
    }

    if (_req.pduType == BerTag::GetBulkRequest) return handleGetBulk(response, capacity, state);

    bool getNext = _req.pduType == BerTag::GetNextRequest;
    SnmpResponseBuilder builder(response, capacity, _req);
    for (uint8_t i = 0; i < _req.varbindCount; i++) {
        const Oid& name = _req.names[i];
        bool added;
        if (getNext) {
            added = mibGetNext(name, state, MibView::V2c, _found)
                        ? builder.add(_found.oid, _found.value)
                        : builder.add(name, SnmpValue::makeException(BerTag::EndOfMibView));
        } else {
            if (mibGet(name, state, _found)) {
                added = builder.add(name, _found.value);
            } else {
                uint8_t ex = mibHasObject(name) ? BerTag::NoSuchInstance : BerTag::NoSuchObject;
                added = builder.add(name, SnmpValue::makeException(ex));
            }
        }
        if (!added) return respondError(_req, SnmpError::TooBig, 0, response, capacity);
    }
    return finishResponse(builder, response, capacity);
}

size_t SnmpAgent::handleGetBulk(uint8_t* response, size_t capacity, const SnmpDeviceState& state) {
    _outcome = SnmpOutcome::RespondedBulk;
    // RFC 3416 4.2.3: negative values count as 0, non-repeaters is capped at the varbind count
    uint8_t n = _req.varbindCount;
    uint8_t nonRepeaters = _req.nonRepeaters <= 0 ? 0 : (_req.nonRepeaters >= n ? n : (uint8_t)_req.nonRepeaters);
    uint32_t maxRepetitions = _req.maxRepetitions <= 0 ? 0 : (uint32_t)_req.maxRepetitions;
    uint8_t repeaters = n - nonRepeaters;

    SnmpResponseBuilder builder(response, capacity, _req);
    bool full = false;

    // Each non-repeater once, like GetNext
    for (uint8_t i = 0; i < nonRepeaters && !full; i++) {
        if (builder.count() >= SNMP_MAX_BULK_VARBINDS) {
            full = true;
            break;
        }
        bool added = mibGetNext(_req.names[i], state, MibView::V2c, _found)
                         ? builder.tryAdd(_found.oid, _found.value)
                         : builder.tryAdd(_req.names[i], SnmpValue::makeException(BerTag::EndOfMibView));
        full = !added;
    }

    // Then rows of the repeaters, each continuing from its previous result
    for (uint8_t j = 0; j < repeaters; j++) _cursor[j] = _req.names[nonRepeaters + j];
    for (uint32_t row = 0; row < maxRepetitions && repeaters > 0 && !full; row++) {
        bool allEnded = true;
        for (uint8_t j = 0; j < repeaters && !full; j++) {
            if (builder.count() >= SNMP_MAX_BULK_VARBINDS) {
                full = true;
                break;
            }
            bool added;
            if (mibGetNext(_cursor[j], state, MibView::V2c, _found)) {
                added = builder.tryAdd(_found.oid, _found.value);
                if (added) _cursor[j] = _found.oid;
                allEnded = false;
            } else {
                added = builder.tryAdd(_cursor[j], SnmpValue::makeException(BerTag::EndOfMibView));
            }
            full = !added;
        }
        // Once every repeater is past the end, further rows would only repeat endOfMibView
        if (allEnded) break;
    }

    // Truncating is allowed; only if not even the first varbind fits is it tooBig
    bool wantedAny = nonRepeaters > 0 || (repeaters > 0 && maxRepetitions > 0);
    if (wantedAny && builder.count() == 0) return respondError(_req, SnmpError::TooBig, 0, response, capacity);
    return finishResponse(builder, response, capacity);
}
