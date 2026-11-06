#include "update_url.h"

#include <string.h>
#include <strings.h>

#include <netconfig.h>

namespace {

bool isHostChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
}

}  // namespace

bool parseUpdateUrl(const char* text, UpdateUrl& out) {
    memset(&out, 0, sizeof(out));
    if (text == nullptr) return false;
    size_t len = strlen(text);
    if (len > UPDATE_URL_MAX || len < 8 || strncasecmp(text, "http://", 7) != 0) return false;
    for (size_t i = 0; i < len; i++) {
        if ((unsigned char)text[i] <= 0x20 || (unsigned char)text[i] >= 0x7F) return false;
    }

    const char* p = text + 7;
    const char* hostEnd;
    if (*p == '[') {
        const char* close = strchr(p, ']');
        if (close == nullptr || close == p + 1 || (size_t)(close - p - 1) >= sizeof(out.host)) return false;
        memcpy(out.host, p + 1, (size_t)(close - p - 1));
        uint8_t addr[16];
        if (!parseIPv6(out.host, addr)) return false;
        out.hostIsIPv6 = true;
        hostEnd = close + 1;
    } else {
        hostEnd = p;
        while (isHostChar(*hostEnd)) hostEnd++;
        size_t hostLen = (size_t)(hostEnd - p);
        if (hostLen == 0 || hostLen >= sizeof(out.host)) return false;
        memcpy(out.host, p, hostLen);
    }

    p = hostEnd;
    out.port = 80;
    if (*p == ':') {
        p++;
        uint32_t port = 0;
        const char* digits = p;
        while (*p >= '0' && *p <= '9' && port <= 65535) port = port * 10 + (uint32_t)(*p++ - '0');
        if (p == digits || port == 0 || port > 65535) return false;
        out.port = (uint16_t)port;
    }
    if (*p != '\0' && *p != '/') return false;  // '@' (userinfo), '_' and the like end here

    size_t pathLen = strlen(p);
    while (pathLen > 0 && p[pathLen - 1] == '/') pathLen--;
    if (pathLen >= sizeof(out.path)) return false;
    memcpy(out.path, p, pathLen);
    return true;
}
