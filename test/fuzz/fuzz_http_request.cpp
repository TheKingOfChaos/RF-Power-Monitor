/**
 * @file fuzz_http_request.cpp
 * @brief Fuzz the web page's request parser (lib/web_ui HttpRequest) as TCP delivers it: in pieces
 *
 * Input: pieces of [length byte][bytes]; the request is fed one piece at a time,
 * as segments arrive. The firmware's stream path (the firmware upload) is set.
 */
#include <http_request.h>

#include "fuzz_common.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static HttpRequest* req = new HttpRequest();  // 2 KB buffer
    req->reset();
    req->streamPath("/firmware/upload");

    FuzzInput in(data, size);
    HttpParse st = HttpParse::Incomplete;
    while (in.left() > 0 && st == HttpParse::Incomplete) {
        size_t len;
        const uint8_t* piece = in.take(in.byte(), len);
        if (len == 0) continue;  // TCP delivers no empty segments; the firmware feeds its own buffer, never nullptr
        std::vector<uint8_t> copy(piece, piece + len);  // exact size for ASan
        st = req->feed(copy.data(), copy.size());
    }
    if (st == HttpParse::Complete || st == HttpParse::Streaming) {
        char value[256];
        req->header("Cookie", value, sizeof(value));
        req->header("Content-Type", value, sizeof(value));
        req->header("X-CSRF-Token", value, sizeof(value));
        req->header("", value, 1);
        if (strlen(req->path()) > HTTP_PATH_MAX) __builtin_trap();
        if (st == HttpParse::Complete) {
            if (req->bodyLength() > HTTP_REQUEST_MAX) __builtin_trap();
            volatile char sink = 0;
            for (size_t i = 0; i < req->bodyLength(); i++) sink ^= req->body()[i];
        } else {
            size_t prefix;
            const uint8_t* p = req->bodyPrefix(prefix);
            if (prefix > HTTP_REQUEST_MAX) __builtin_trap();
            volatile uint8_t sink = 0;
            for (size_t i = 0; i < prefix; i++) sink ^= p[i];
        }
    }
    return 0;
}
