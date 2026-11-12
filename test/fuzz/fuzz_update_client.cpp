/**
 * @file fuzz_update_client.cpp
 * @brief Fuzz what the device reads from the update site (lib/update_client)
 *
 * Input:
 *   byte 0  bit 0: sign the body with the shared key (as the site does), so the
 *           code behind the MAC check is reached; bit 1: body limit UPDATE_REPLY_MAX
 *           (a check-in) instead of a firmware download
 *   byte 1  piece size for the response head parser (0 = everything at once)
 *   rest    the HTTP response: head, then the body
 *
 * The same bytes are also read as an update site URL (setting, web form, DHCP 225).
 */
#include <string>

#include <http_response.h>
#include <update_protocol.h>
#include <update_url.h>

#include "fuzz_common.h"

static const uint8_t KEY[UPDATE_KEY_LEN] = {
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
static const char NONCE[] = "00112233445566778899aabbccddeeff";

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    FuzzInput in(data, size);
    uint8_t mode = in.byte();
    size_t piece = in.byte();
    size_t len;
    const uint8_t* resp = in.rest(len);
    if (piece == 0) piece = len > 0 ? len : 1;

    HttpResponseParser parser;
    parser.reset((mode & 2) ? UPDATE_REPLY_MAX : 2 * 0xD7000u + 64);
    size_t off = 0;
    HttpRespParse st = HttpRespParse::Incomplete;
    while (off < len && st == HttpRespParse::Incomplete) {
        size_t n = len - off < piece ? len - off : piece;
        std::vector<uint8_t> copy(resp + off, resp + off + n);  // exact size for ASan
        size_t consumed = 0;
        st = parser.feed(copy.data(), copy.size(), consumed);
        if (consumed > n) __builtin_trap();
        off += consumed;
        if (st == HttpRespParse::Incomplete && consumed != n) __builtin_trap();
    }
    if (st == HttpRespParse::HeadDone) {
        // Three digits; callers act only on 200, any other code is an error to them
        if (parser.status() < 0 || parser.status() > 999) __builtin_trap();
        if ((mode & 2) && parser.contentLength() > UPDATE_REPLY_MAX) __builtin_trap();
    }

    // The body (what follows the head, or all of it) as a check-in reply
    std::string body((const char*)resp + off, len - off);
    if (mode & 1) {
        if (!body.empty() && body.back() != '\n') body += '\n';
        std::string signedPart = std::string(NONCE) + "\n" + body;
        char mac[UPDATE_MAC_HEX + 1];
        updateMacHex(KEY, (const uint8_t*)signedPart.data(), signedPart.size(), mac);
        body += "auth=";
        body += mac;
        body += "\n";
    }
    std::vector<char> b(body.begin(), body.end());  // exact size for ASan
    CheckinReply reply;
    ReplyKind kind = parseCheckinReply(b.data(), b.size(), NONCE, KEY, reply);
    if (kind == ReplyKind::Update) {
        if (reply.path[0] != '/' || strnlen(reply.version, sizeof(reply.version)) == sizeof(reply.version) ||
            strnlen(reply.path, sizeof(reply.path)) == sizeof(reply.path)) {
            __builtin_trap();
        }
    }
    char word[24];
    replyStatusWord(b.data(), b.size(), word, sizeof(word));
    for (const char* p = word; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-')) __builtin_trap();
    }

    std::vector<char> text = fuzzString(resp, len);
    UpdateUrl url;
    if (parseUpdateUrl(text.data(), url)) {
        if (url.port == 0 || strnlen(url.host, sizeof(url.host)) == sizeof(url.host)) __builtin_trap();
    }
    uint8_t key[UPDATE_KEY_LEN];
    parseKeyHex(text.data(), key);
    return 0;
}
