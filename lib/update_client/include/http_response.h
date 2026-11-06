#pragma once

/**
 * @file http_response.h
 * @brief The head of an HTTP/1.x response (status line and headers), read in
 *        pieces as TCP delivers it. The body is left to the caller.
 *
 * Only responses with a Content-Length are accepted (the update site always
 * sends one); chunked transfer encoding, a missing or repeated length, a length
 * over the caller's limit and heads over HTTP_RESPONSE_HEAD_MAX bytes are refused.
 * Lines may end in CRLF or LF. Pure C++, host tested.
 */

#include <stddef.h>
#include <stdint.h>

/// Largest response head accepted (status line, headers, blank line)
static const size_t HTTP_RESPONSE_HEAD_MAX = 1024;

/// State of a response head (named apart from web_ui's HttpParse for requests)
enum class HttpRespParse : uint8_t { Incomplete, HeadDone, Malformed };

/// Reads one response head
class HttpResponseParser {
public:
    /// Start a new response; a Content-Length over maxBody is refused
    void reset(size_t maxBody);

    /**
     * @brief Feed received bytes
     * @param consumed Set to the bytes that belonged to the head; the rest is body
     * @return HeadDone once the blank line was read (later calls consume nothing)
     */
    HttpRespParse feed(const uint8_t* data, size_t len, size_t& consumed);

    /// Status code (valid after HeadDone)
    int status() const { return _status; }

    /// Body length (valid after HeadDone)
    uint32_t contentLength() const { return _contentLength; }

private:
    HttpRespParse endLine();

    HttpRespParse _state = HttpRespParse::Incomplete;
    size_t _maxBody = 0;
    size_t _headBytes = 0;
    char _line[128] = {};   ///< Current line; longer lines keep their start only
    size_t _lineLen = 0;
    bool _lineTooLong = false;
    bool _statusRead = false;
    bool _haveLength = false;
    int _status = 0;
    uint32_t _contentLength = 0;
};
