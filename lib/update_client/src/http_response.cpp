#include "http_response.h"

#include <string.h>
#include <strings.h>

void HttpResponseParser::reset(size_t maxBody) {
    *this = HttpResponseParser();
    _maxBody = maxBody;
}

HttpRespParse HttpResponseParser::feed(const uint8_t* data, size_t len, size_t& consumed) {
    consumed = 0;
    while (_state == HttpRespParse::Incomplete && consumed < len) {
        char c = (char)data[consumed++];
        if (++_headBytes > HTTP_RESPONSE_HEAD_MAX) return _state = HttpRespParse::Malformed;
        if (c == '\n') {
            if (_lineLen > 0 && _line[_lineLen - 1] == '\r') _lineLen--;
            _line[_lineLen] = '\0';
            _state = endLine();
            _lineLen = 0;
            _lineTooLong = false;
        } else if (_lineLen < sizeof(_line) - 1) {
            _line[_lineLen++] = c;
        } else {
            _lineTooLong = true;
        }
    }
    return _state;
}

HttpRespParse HttpResponseParser::endLine() {
    if (!_statusRead) {
        // "HTTP/1.x NNN reason"
        if (_lineTooLong || strncmp(_line, "HTTP/1.", 7) != 0 || _line[7] < '0' || _line[7] > '9' ||
            _line[8] != ' ') {
            return HttpRespParse::Malformed;
        }
        const char* code = _line + 9;
        for (int i = 0; i < 3; i++) {
            if (code[i] < '0' || code[i] > '9') return HttpRespParse::Malformed;
        }
        if (code[3] != '\0' && code[3] != ' ') return HttpRespParse::Malformed;
        _status = (code[0] - '0') * 100 + (code[1] - '0') * 10 + (code[2] - '0');
        _statusRead = true;
        return HttpRespParse::Incomplete;
    }
    if (_lineLen == 0) {  // blank line: end of head
        if (!_haveLength || _contentLength > _maxBody) return HttpRespParse::Malformed;
        return HttpRespParse::HeadDone;
    }
    if (strncasecmp(_line, "transfer-encoding:", 18) == 0) return HttpRespParse::Malformed;
    if (strncasecmp(_line, "content-length:", 15) == 0) {
        if (_haveLength || _lineTooLong) return HttpRespParse::Malformed;
        const char* p = _line + 15;
        while (*p == ' ' || *p == '\t') p++;
        uint64_t n = 0;
        const char* digits = p;
        while (*p >= '0' && *p <= '9') {
            n = n * 10 + (uint64_t)(*p++ - '0');
            if (n > 0xFFFFFFFFu) return HttpRespParse::Malformed;
        }
        while (*p == ' ' || *p == '\t') p++;
        if (p == digits || *p != '\0') return HttpRespParse::Malformed;
        _contentLength = (uint32_t)n;
        _haveLength = true;
    }
    return HttpRespParse::Incomplete;
}
