#include "http_fetch.h"

#include <stdio.h>
#include <string.h>

#include <new>

#include <lwip/dns.h>
#include <lwip/ip_addr.h>
#include <lwip/pbuf.h>
#include <lwip/priv/tcp_priv.h>
#include <lwip/tcp.h>

#include <http_response.h>

#include "net_stats.h"  // DriverLock

namespace {

/// Receive window for fetches (see onConnected)
const tcpwnd_size_t FETCH_RCV_WND = 2 * TCP_MSS;

// State shared with the lwIP callbacks, which run in lwIP context; loop() reads
// and changes it with DriverLock held
FetchState state = FetchState::Idle;
tcp_pcb* pcb = nullptr;
pbuf* rx = nullptr;          // received, not yet read
bool remoteClosed = false;
const char* error = "";
uint32_t generation = 0;     // tells a late DNS answer for an aborted fetch apart

// loop() only
ip_addr_t addr;
uint16_t port = 0;
char* request = nullptr;     // heap, while the fetch runs
size_t requestLen = 0;
size_t requestSent = 0;
HttpResponseParser parser;
uint32_t bodyLeft = 0;
uint32_t startMs = 0, lastActivityMs = 0, totalLimitMs = 0;
volatile bool activity = false;  // set by callbacks, folded into lastActivityMs by poll

void fail(const char* why) {
    error = why;
    state = FetchState::Failed;
}

/// Close or abort the connection and drop received data (lock held)
void releaseConnection(bool graceful) {
    if (pcb != nullptr) {
        tcp_arg(pcb, nullptr);
        tcp_recv(pcb, nullptr);
        tcp_sent(pcb, nullptr);
        tcp_err(pcb, nullptr);
        if (!graceful || tcp_close(pcb) != ERR_OK) tcp_abort(pcb);
        pcb = nullptr;
    }
    if (rx != nullptr) {
        pbuf_free(rx);
        rx = nullptr;
    }
}

void freeRequest() {
    delete[] request;
    request = nullptr;
}

/// Queue as much of the request as the send buffer takes (lock held)
void sendMore() {
    if (pcb == nullptr || request == nullptr) return;
    while (requestSent < requestLen) {
        size_t room = tcp_sndbuf(pcb);
        if (room == 0) break;
        size_t n = requestLen - requestSent;
        if (n > room) n = room;
        if (n > 0xFFFF) n = 0xFFFF;
        if (tcp_write(pcb, request + requestSent, (u16_t)n, 0) != ERR_OK) break;  // retried from the sent callback
        requestSent += n;
    }
    tcp_output(pcb);
    if (requestSent == requestLen && state == FetchState::Sending) state = FetchState::Head;
}

err_t onSent(void*, tcp_pcb*, u16_t) {
    activity = true;
    sendMore();
    return ERR_OK;
}

err_t onRecv(void*, tcp_pcb* tpcb, pbuf* p, err_t err) {
    if (tpcb != pcb) {
        if (p != nullptr) pbuf_free(p);
        return ERR_OK;
    }
    activity = true;
    if (p == nullptr || err != ERR_OK) {
        if (p != nullptr) pbuf_free(p);
        remoteClosed = true;
        return ERR_OK;
    }
    if (rx == nullptr) {
        rx = p;
    } else {
        pbuf_cat(rx, p);  // the window is only opened as data is read (httpFetchRead)
    }
    return ERR_OK;
}

void onErr(void*, err_t) {
    pcb = nullptr;  // lwIP has freed it
    if (state != FetchState::Done && state != FetchState::Failed) fail("connection reset");
}

err_t onConnected(void*, tcp_pcb* tpcb, err_t err) {
    if (tpcb != pcb) return ERR_OK;
    activity = true;
    if (err != ERR_OK) {
        fail("connect failed");
        return ERR_OK;
    }
    // A smaller receive window than lwIP's TCP_WND (8 segments): a full window
    // arriving at once overruns the W5500 and drops other frames (SNMP requests).
    // Set before the ACK of the SYN-ACK goes out; reads re-open only what they take.
    tpcb->rcv_wnd = tpcb->rcv_ann_wnd = FETCH_RCV_WND;
    tpcb->rcv_ann_right_edge = tpcb->rcv_nxt + FETCH_RCV_WND;
    state = FetchState::Sending;
    sendMore();
    return ERR_OK;
}

/// Open the connection to addr:port (lock held)
void connectNow() {
    pcb = tcp_new_ip_type(IP_GET_TYPE(&addr));
    if (pcb == nullptr) {
        fail("out of memory");
        return;
    }
    tcp_recv(pcb, onRecv);
    tcp_sent(pcb, onSent);
    tcp_err(pcb, onErr);
    state = FetchState::Connecting;
    if (tcp_connect(pcb, &addr, port, onConnected) != ERR_OK) {
        releaseConnection(false);
        fail("connect failed");
    }
}

void onDnsFound(const char*, const ip_addr_t* found, void* arg) {
    if ((uint32_t)(uintptr_t)arg != generation || state != FetchState::Resolving) return;  // aborted meanwhile
    if (found == nullptr) {
        fail("dns failed");
        return;
    }
    ip_addr_copy(addr, *found);
    connectNow();
}

/// Bytes waiting in rx (lock held)
size_t rxLen() {
    return rx == nullptr ? 0 : rx->tot_len;
}

/// Copy up to max bytes from rx and drop them, opening the window by as much (lock held)
size_t takeRx(uint8_t* buf, size_t max) {
    size_t n = rxLen();
    if (n > max) n = max;
    if (n == 0) return 0;
    pbuf_copy_partial(rx, buf, (u16_t)n, 0);
    rx = pbuf_free_header(rx, (u16_t)n);
    if (pcb != nullptr) tcp_recved(pcb, (u16_t)n);
    return n;
}

/// Read the response head from rx (lock held)
void readHead() {
    uint8_t chunk[64];
    while (state == FetchState::Head && rxLen() > 0) {
        size_t n = rxLen() < sizeof(chunk) ? rxLen() : sizeof(chunk);
        pbuf_copy_partial(rx, chunk, (u16_t)n, 0);
        size_t used = 0;
        HttpRespParse r = parser.feed(chunk, n, used);
        rx = pbuf_free_header(rx, (u16_t)used);
        if (pcb != nullptr) tcp_recved(pcb, (u16_t)used);
        if (r == HttpRespParse::Malformed) {
            fail("bad response");
        } else if (r == HttpRespParse::HeadDone) {
            bodyLeft = parser.contentLength();
            state = FetchState::Body;
        }
    }
}

/// The whole body was read: close the connection politely and free the request (lock held)
void finishIfComplete() {
    if (state == FetchState::Body && bodyLeft == 0) {
        releaseConnection(true);
        freeRequest();
        state = FetchState::Done;
    }
}

}  // namespace

bool httpFetchStart(const UpdateUrl& url, const char* method, const char* path, const char* extraHeaders,
                    const uint8_t* body, size_t bodyLen, size_t maxBody, uint32_t nowMs, uint32_t totalMs) {
    httpFetchAbort();
    char host[72];
    if (url.port == 80) {
        snprintf(host, sizeof(host), url.hostIsIPv6 ? "[%s]" : "%s", url.host);
    } else {
        snprintf(host, sizeof(host), url.hostIsIPv6 ? "[%s]:%u" : "%s:%u", url.host, (unsigned)url.port);
    }
    const char* headFormat = "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nContent-Length: %u\r\n%s\r\n";
    if (extraHeaders == nullptr) extraHeaders = "";
    int headLen = snprintf(nullptr, 0, headFormat, method, path, host, (unsigned)bodyLen, extraHeaders);
    if (headLen <= 0) {
        fail("bad request");
        return false;
    }
    requestLen = (size_t)headLen + bodyLen;
    request = new (std::nothrow) char[requestLen + 1];
    if (request == nullptr) {
        fail("out of memory");
        return false;
    }
    snprintf(request, (size_t)headLen + 1, headFormat, method, path, host, (unsigned)bodyLen, extraHeaders);
    if (bodyLen > 0) memcpy(request + headLen, body, bodyLen);
    requestSent = 0;
    parser.reset(maxBody);
    bodyLeft = 0;
    startMs = lastActivityMs = nowMs;
    totalLimitMs = totalMs;
    port = url.port;

    DriverLock lock;
    generation++;
    error = "";
    remoteClosed = false;
    activity = false;
    if (ipaddr_aton(url.host, &addr)) {  // a literal IPv4 or IPv6 address
        connectNow();
    } else {
        state = FetchState::Resolving;
        err_t r = dns_gethostbyname(url.host, &addr, onDnsFound, (void*)(uintptr_t)generation);
        if (r == ERR_OK) {
            connectNow();  // cached
        } else if (r != ERR_INPROGRESS) {
            fail("dns failed");
        }
    }
    if (state == FetchState::Failed) {
        releaseConnection(false);
        freeRequest();
        return false;
    }
    return true;
}

FetchState httpFetchPoll(uint32_t nowMs) {
    DriverLock lock;
    if (state == FetchState::Idle || state == FetchState::Done) return state;
    if (state == FetchState::Failed) {  // e.g. failed in a callback: free what is left
        releaseConnection(false);
        freeRequest();
        return state;
    }
    if (activity) {
        activity = false;
        lastActivityMs = nowMs;
    }
    if (state == FetchState::Sending) sendMore();
    if (state == FetchState::Head) readHead();
    if (state == FetchState::Body) finishIfComplete();

    if (state != FetchState::Done && state != FetchState::Failed) {
        bool connecting = state == FetchState::Resolving || state == FetchState::Connecting;
        if (connecting && nowMs - startMs >= FETCH_CONNECT_TIMEOUT_MS) {
            fail(state == FetchState::Resolving ? "dns timeout" : "connect timeout");
        } else if (totalLimitMs != 0 && nowMs - startMs >= totalLimitMs) {
            fail("timeout");
        } else if (!connecting && nowMs - lastActivityMs >= FETCH_IDLE_TIMEOUT_MS) {
            fail("no answer");
        } else if (remoteClosed && rxLen() == 0) {
            fail("connection closed");
        }
    }
    if (state == FetchState::Failed) {
        releaseConnection(false);
        freeRequest();
    }
    return state;
}

int httpFetchStatus() {
    return parser.status();
}

uint32_t httpFetchContentLength() {
    return parser.contentLength();
}

size_t httpFetchRead(uint8_t* buf, size_t max) {
    DriverLock lock;
    if (state != FetchState::Body) return 0;
    if (max > bodyLeft) max = bodyLeft;
    if (max > 0xFFFF) max = 0xFFFF;
    size_t n = takeRx(buf, max);
    bodyLeft -= (uint32_t)n;
    finishIfComplete();
    return n;
}

const char* httpFetchError() {
    return error;
}

void httpFetchAbort() {
    DriverLock lock;
    generation++;
    releaseConnection(false);
    freeRequest();
    state = FetchState::Idle;
    error = "";
}
