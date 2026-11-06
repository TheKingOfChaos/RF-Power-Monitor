#include "http_request.h"

#include <stdio.h>
#include <string.h>

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/// Case-insensitive compare of len bytes
static bool equalsNoCase(const char* a, const char* b, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

void HttpRequest::reset() {
    _len = 0;
    _headEnd = 0;
    _bodyStart = 0;
    _contentLength = 0;
    _state = HttpParse::Incomplete;
    _method = HttpMethod::Other;
    _path[0] = '\0';
    _buf[0] = '\0';
}

HttpParse HttpRequest::feed(const uint8_t* data, size_t len) {
    if (_state != HttpParse::Incomplete) return _state;
    if (len > HTTP_REQUEST_MAX - _len) {
        _state = HttpParse::TooLarge;
        return _state;
    }
    const size_t before = _len;
    memcpy(_buf + _len, data, len);
    _len += len;
    _buf[_len] = '\0';

    if (_headEnd == 0) {
        // Find the end of the head with a length-bounded search: body bytes that arrived in the
        // same read may contain anything, NUL included
        size_t headLen = _len;
        bool found = false;
        for (size_t i = before > 3 ? before - 3 : 0; i + 4 <= _len; i++) {
            if (memcmp(_buf + i, "\r\n\r\n", 4) == 0) {
                headLen = i + 4;
                found = true;
                break;
            }
        }
        // A NUL byte in the head would cut the strings parsed below
        if (memchr(_buf, '\0', headLen) != nullptr) {
            _state = HttpParse::Bad;
            return _state;
        }
        if (!found) return _state;
        _headEnd = headLen;
        _state = parseHead();
        if (_state == HttpParse::Streaming && _len - _bodyStart > _contentLength) _state = HttpParse::Bad;
        if (_state != HttpParse::Incomplete) return _state;
    }
    if (_len - _bodyStart >= _contentLength) {
        if (_len - _bodyStart > _contentLength) {
            _state = HttpParse::Bad;  // more bytes than announced: no pipelining here
        } else if (memchr(_buf + _bodyStart, '\0', _contentLength) != nullptr) {
            _state = HttpParse::Bad;  // the form parsers work on strings: no NUL in a buffered body
        } else {
            _state = HttpParse::Complete;
        }
    }
    return _state;
}

HttpParse HttpRequest::parseHead() {
    // Request line: METHOD SP target SP HTTP/x.y
    const char* line = _buf;
    const char* lineEnd = strstr(line, "\r\n");
    const char* sp1 = (const char*)memchr(line, ' ', (size_t)(lineEnd - line));
    if (sp1 == nullptr) return HttpParse::Bad;
    const char* target = sp1 + 1;
    const char* sp2 = (const char*)memchr(target, ' ', (size_t)(lineEnd - target));
    if (sp2 == nullptr || (size_t)(lineEnd - sp2) < 9 || strncmp(sp2 + 1, "HTTP/1.", 7) != 0) return HttpParse::Bad;

    size_t methodLen = (size_t)(sp1 - line);
    if (methodLen == 3 && strncmp(line, "GET", 3) == 0) {
        _method = HttpMethod::Get;
    } else if (methodLen == 4 && strncmp(line, "POST", 4) == 0) {
        _method = HttpMethod::Post;
    } else {
        _method = HttpMethod::Other;
    }

    size_t targetLen = (size_t)(sp2 - target);
    const char* query = (const char*)memchr(target, '?', targetLen);
    if (query != nullptr) targetLen = (size_t)(query - target);
    if (targetLen == 0 || target[0] != '/') return HttpParse::Bad;
    if (targetLen > HTTP_PATH_MAX) targetLen = HTTP_PATH_MAX;  // unknown anyway: answers 404
    memcpy(_path, target, targetLen);
    _path[targetLen] = '\0';

    char value[24];
    if (header("Transfer-Encoding", value, sizeof(value))) return HttpParse::Bad;
    _contentLength = 0;
    bool stream = _method == HttpMethod::Post && _streamPath != nullptr && strcmp(_path, _streamPath) == 0;
    bool haveLength = header("Content-Length", value, sizeof(value));
    if (haveLength) {
        if (value[0] == '\0' || strlen(value) > (stream ? 9u : 6u)) return value[0] ? HttpParse::TooLarge : HttpParse::Bad;
        for (const char* c = value; *c; c++) {
            if (*c < '0' || *c > '9') return HttpParse::Bad;
            _contentLength = _contentLength * 10 + (size_t)(*c - '0');
        }
    }
    _bodyStart = _headEnd;
    if (stream) {
        if (_contentLength == 0) return HttpParse::Bad;
        return _contentLength > HTTP_STREAM_MAX ? HttpParse::TooLarge : HttpParse::Streaming;
    }
    if (_contentLength > HTTP_REQUEST_MAX - _bodyStart) return HttpParse::TooLarge;
    return HttpParse::Incomplete;
}

bool HttpRequest::header(const char* name, char* out, size_t outSize) const {
    if (_headEnd == 0 || outSize == 0) return false;
    size_t nameLen = strlen(name);
    // Header lines start after the request line and end at the blank line
    const char* p = strstr(_buf, "\r\n");
    const char* end = _buf + _headEnd - 2;
    while (p != nullptr && p < end) {
        p += 2;
        const char* eol = strstr(p, "\r\n");
        if (eol == nullptr || eol > end) break;
        if ((size_t)(eol - p) > nameLen && p[nameLen] == ':' && equalsNoCase(p, name, nameLen)) {
            const char* v = p + nameLen + 1;
            while (v < eol && (*v == ' ' || *v == '\t')) v++;
            const char* vEnd = eol;
            while (vEnd > v && (vEnd[-1] == ' ' || vEnd[-1] == '\t')) vEnd--;
            size_t n = (size_t)(vEnd - v);
            if (n >= outSize) return false;
            memcpy(out, v, n);
            out[n] = '\0';
            return true;
        }
        p = eol;
    }
    return false;
}

static int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool formValue(const char* body, size_t len, const char* name, char* out, size_t outSize) {
    if (body == nullptr || name == nullptr || out == nullptr || outSize == 0) return false;
    size_t nameLen = strlen(name);
    size_t pos = 0;
    while (pos <= len) {
        size_t end = pos;
        while (end < len && body[end] != '&') end++;
        const char* field = body + pos;
        size_t fieldLen = end - pos;
        if (fieldLen > nameLen && field[nameLen] == '=' && strncmp(field, name, nameLen) == 0) {
            size_t o = 0;
            for (size_t i = nameLen + 1; i < fieldLen; i++) {
                char c = field[i];
                if (c == '+') {
                    c = ' ';
                } else if (c == '%') {
                    if (i + 2 >= fieldLen) return false;  // needs two hex digits
                    int hi = hexDigit(field[i + 1]), lo = hexDigit(field[i + 2]);
                    if (hi < 0 || lo < 0) return false;
                    c = (char)(hi << 4 | lo);
                    i += 2;
                }
                if (c == '\0' || o + 1 >= outSize) return false;
                out[o++] = c;
            }
            out[o] = '\0';
            return true;
        }
        if (fieldLen == nameLen && strncmp(field, name, nameLen) == 0) {
            out[0] = '\0';  // "name" without '=' counts as empty
            return true;
        }
        pos = end + 1;
    }
    return false;
}

bool cookieValue(const char* cookies, const char* name, char* out, size_t outSize) {
    if (cookies == nullptr || outSize == 0) return false;
    size_t nameLen = strlen(name);
    const char* p = cookies;
    while (*p) {
        while (*p == ' ' || *p == ';') p++;
        const char* end = strchr(p, ';');
        if (end == nullptr) end = p + strlen(p);
        if ((size_t)(end - p) > nameLen && p[nameLen] == '=' && strncmp(p, name, nameLen) == 0) {
            const char* v = p + nameLen + 1;
            size_t n = (size_t)(end - v);
            if (n >= outSize) return false;
            memcpy(out, v, n);
            out[n] = '\0';
            return true;
        }
        p = end;
    }
    return false;
}

void TextBuffer::add(const char* text, size_t len) {
    if (!_ok) return;
    if (len >= _size - _len) {
        _ok = false;
        return;
    }
    memcpy(_buf + _len, text, len);
    _len += len;
    _buf[_len] = '\0';
}

void TextBuffer::add(const char* text) { add(text, strlen(text)); }

void TextBuffer::addEscaped(const char* text) {
    for (const char* c = text; *c; c++) {
        switch (*c) {
        case '&': add("&amp;"); break;
        case '<': add("&lt;"); break;
        case '>': add("&gt;"); break;
        case '"': add("&quot;"); break;
        case '\'': add("&#39;"); break;
        default: add(c, 1); break;
        }
    }
}

void TextBuffer::addUnsigned(uint32_t value) {
    char num[12];
    snprintf(num, sizeof(num), "%lu", (unsigned long)value);
    add(num);
}

void StreamBody::begin(uint32_t length, uint32_t nowMs) {
    _left = length;
    _startMs = nowMs;
    _lastDataMs = nowMs;
}

void StreamBody::consumed(uint32_t n, uint32_t nowMs) {
    _left = n >= _left ? 0 : _left - n;
    _lastDataMs = nowMs;
}

bool StreamBody::expired(uint32_t nowMs) const {
    if (done()) return false;
    return nowMs - _lastDataMs > STREAM_IDLE_TIMEOUT_MS || nowMs - _startMs > STREAM_TOTAL_TIMEOUT_MS;
}
