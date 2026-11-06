#pragma once

/**
 * @file http_fetch.h
 * @brief One HTTP/1.1 request at a time to the update server, on lwIP's raw TCP
 *        and DNS APIs, so loop() on core 0 never waits for the network.
 *
 * httpFetchStart() resolves the host and connects in the background;
 * httpFetchPoll() advances timeouts and reads the response head
 * (lib/update_client HttpResponseParser); the body is taken with
 * httpFetchRead(). Received data stays in lwIP's buffers until it is read, and
 * the TCP window opens only as the caller reads (as WiFiClient does), so a slow
 * reader (flash writes) slows the sender instead of using more memory.
 * The request is on the heap while the fetch runs. Core 0 only.
 */

#include <stddef.h>
#include <stdint.h>
#include <update_url.h>

/// Where a fetch is
enum class FetchState : uint8_t { Idle, Resolving, Connecting, Sending, Head, Body, Done, Failed };

/// Resolve and connect within this time
static const uint32_t FETCH_CONNECT_TIMEOUT_MS = 10000;
/// Fail when nothing was sent or received for this long
static const uint32_t FETCH_IDLE_TIMEOUT_MS = 30000;

/**
 * @brief Start a request (any fetch still running is aborted first)
 * @param path Absolute path, sent as is (the caller prefixes the URL's path)
 * @param extraHeaders More header lines, each ending in "\r\n" (or nullptr)
 * @param body Request body (nullptr when bodyLen is 0)
 * @param maxBody Longest response body accepted
 * @param totalMs Fail the whole fetch after this long (0 = only the connect and idle timeouts)
 * @return false when the request cannot be built or memory is short (httpFetchError() says why)
 */
bool httpFetchStart(const UpdateUrl& url, const char* method, const char* path, const char* extraHeaders,
                    const uint8_t* body, size_t bodyLen, size_t maxBody, uint32_t nowMs, uint32_t totalMs);

/// Advance the fetch (timeouts, response head); call every loop() pass
FetchState httpFetchPoll(uint32_t nowMs);

/// The response's status code (from Body on)
int httpFetchStatus();

/// The response's Content-Length (from Body on)
uint32_t httpFetchContentLength();

/**
 * @brief Take body bytes received so far (Body state only)
 * @return Bytes copied; the fetch becomes Done once the whole body was read
 */
size_t httpFetchRead(uint8_t* buf, size_t max);

/// Why the fetch failed, e.g. "dns failed", "connect timeout", "connection closed"
const char* httpFetchError();

/// Stop the fetch and free everything; the state returns to Idle
void httpFetchAbort();

/**
 * @brief A fetch is in use (running, or ended and not yet released with httpFetchAbort())
 *
 * There is one fetch: its two users (update service, the fetchtest menu command)
 * start one only when this is false, so neither takes the other's connection away.
 */
bool httpFetchBusy();
