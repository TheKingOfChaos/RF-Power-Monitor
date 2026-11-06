#pragma once

/**
 * @file http_request.h
 * @brief Incremental, bounded HTTP/1.x request parser plus form, cookie and
 *        HTML helpers for the configuration page. Pure C++, host tested.
 *
 * The parser never blocks and never allocates: bytes are fed as they arrive
 * into a fixed buffer, and the request is parsed once the headers and the
 * Content-Length body are complete. Anything larger than HTTP_REQUEST_MAX,
 * chunked bodies and malformed request lines are rejected.
 *
 * One path can be named with streamPath(): a POST to it is not buffered. The
 * parser stops after the headers (state Streaming) and the caller reads the
 * body from the connection itself, starting with bodyPrefix(), the body bytes
 * that arrived together with the headers. It is used for the firmware upload.
 */

#include <stddef.h>
#include <stdint.h>

static const size_t HTTP_REQUEST_MAX = 2048;
static const size_t HTTP_PATH_MAX = 64;
/// Largest Content-Length on the stream path: an update file holds two slot images
/// (2 x 0xD7000 bytes, lib/fw_image fw_layout.h) and a 64 byte header
static const size_t HTTP_STREAM_MAX = 0xD7000u * 2 + 64;

enum class HttpMethod : uint8_t { Other, Get, Post };

enum class HttpParse : uint8_t {
    Incomplete,  ///< Need more bytes
    Complete,    ///< Request ready: method(), path(), header(), body()
    Streaming,   ///< Headers of a POST to the stream path are in; the body is the caller's to read
    TooLarge,    ///< Headers or body exceed HTTP_REQUEST_MAX (answer 413)
    Bad          ///< Malformed or unsupported (answer 400)
};

class HttpRequest {
public:
    HttpRequest() { reset(); }

    /// Forget the current request, ready for the next connection (the stream path stays)
    void reset();

    /**
     * @brief The path whose POST body is not buffered (nullptr: none). The string must stay valid.
     *        Needs a Content-Length of 1 to HTTP_STREAM_MAX; none or 0 is Bad, more is TooLarge.
     */
    void streamPath(const char* path) { _streamPath = path; }

    /// Add received bytes; returns the state after them
    HttpParse feed(const uint8_t* data, size_t len);

    HttpParse state() const { return _state; }
    HttpMethod method() const { return _method; }
    /// Path without the query string, e.g. "/network"
    const char* path() const { return _path; }

    /**
     * @brief Value of a header (case-insensitive name), or nullptr
     * @param out Receives the value without surrounding spaces
     */
    bool header(const char* name, char* out, size_t outSize) const;

    const char* body() const { return _buf + _bodyStart; }
    size_t bodyLength() const { return _contentLength; }

    /// Announced body size (the Content-Length), also in state Streaming
    size_t contentLength() const { return _contentLength; }

    /// In state Streaming: the body bytes that came with the headers (len may be 0)
    const uint8_t* bodyPrefix(size_t& len) const {
        len = _len - _bodyStart;
        return (const uint8_t*)_buf + _bodyStart;
    }

private:
    HttpParse parseHead();

    char _buf[HTTP_REQUEST_MAX + 1];
    size_t _len;
    size_t _headEnd;      ///< Index just after "\r\n\r\n", 0 until seen
    size_t _bodyStart;
    size_t _contentLength;
    HttpParse _state;
    HttpMethod _method;
    const char* _streamPath = nullptr;
    char _path[HTTP_PATH_MAX + 1];
};

/**
 * @brief Find a field in an application/x-www-form-urlencoded body and decode it
 *
 * '+' becomes a space and %XX its byte. Returns false if the field is missing,
 * does not fit out (including the NUL), has a bad %-escape or decodes to a NUL.
 */
bool formValue(const char* body, size_t len, const char* name, char* out, size_t outSize);

/**
 * @brief Find a cookie in a Cookie header value ("a=1; sid=abc")
 */
bool cookieValue(const char* cookies, const char* name, char* out, size_t outSize);

/**
 * @brief Bounded text builder for responses; once full it stays full and ok() is false
 */
class TextBuffer {
public:
    TextBuffer(char* buf, size_t size) : _buf(buf), _size(size), _len(0), _ok(size > 0) {
        if (size) buf[0] = '\0';
    }

    void add(const char* text);
    void add(const char* text, size_t len);
    /// Add text with &, <, >, " and ' escaped for HTML text and attribute values
    void addEscaped(const char* text);
    void addUnsigned(uint32_t value);

    const char* c_str() const { return _buf; }
    size_t length() const { return _len; }
    bool ok() const { return _ok; }

private:
    char* _buf;
    size_t _size;
    size_t _len;
    bool _ok;
};

/// A streamed body must keep coming: a pause this long without data ends it
static const uint32_t STREAM_IDLE_TIMEOUT_MS = 30000;
/// and the whole body must be in within this time (a full update file takes well under a minute)
static const uint32_t STREAM_TOTAL_TIMEOUT_MS = 5u * 60u * 1000u;

/**
 * @brief Reading a streamed request body (HttpParse::Streaming) to its end
 *
 * Counts the bytes still to come and says when to give up: after
 * STREAM_IDLE_TIMEOUT_MS without data, or STREAM_TOTAL_TIMEOUT_MS after
 * begin(), so a client sending a byte now and then cannot hold the single
 * connection for long. Also used to read and drop the body of a refused
 * upload, so the client sees the answer instead of a reset connection.
 * Times are millis() values; wrap-around is handled.
 */
class StreamBody {
public:
    /// Expect `length` body bytes (Content-Length minus what came with the headers), starting now
    void begin(uint32_t length, uint32_t nowMs);
    /// n more bytes were read (never counts below zero)
    void consumed(uint32_t n, uint32_t nowMs);
    /// Body bytes not read yet
    uint32_t left() const { return _left; }
    /// The whole body was read
    bool done() const { return _left == 0; }
    /// Not done and silent too long, or past the total deadline
    bool expired(uint32_t nowMs) const;

private:
    uint32_t _left = 0;
    uint32_t _startMs = 0;
    uint32_t _lastDataMs = 0;
};
