/**
 * @file fuzz_dhcp.cpp
 * @brief Fuzz the DHCPv4 frame readers and editor (lib/dhcp_option)
 *
 * Input: byte 0 (bit 0 set: keep the checksums as given, else make them valid
 * so the option walk is reached), then an Ethernet frame. Read for the options
 * the firmware uses: 224 (trap destination), 225 (update site URL), 7 (syslog),
 * 42 (NTP); and edited as an outgoing request asking for 224.
 */
#include <dhcp_option.h>

#include "fuzz_common.h"

static const uint8_t MAC[6] = {0x1E, 0xB2, 0xCF, 0x4E, 0xA2, 0x2C};

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    FuzzInput in(data, size);
    bool raw = in.byte() & 1;
    size_t len;
    const uint8_t* frame = in.rest(len);
    std::vector<uint8_t> f(frame, frame + len);  // exact size for ASan
    if (!raw) fuzzFixIPv4(f.data(), f.size());

    DhcpAckInfo info;
    dhcpReadAck(f.data(), f.size(), MAC, 224, info);
    dhcpReadAckFirstAddress(f.data(), f.size(), MAC, 7, info);
    dhcpReadAckFirstAddress(f.data(), f.size(), MAC, 42, info);
    char text[128];
    if (dhcpAckText(f.data(), f.size(), 225, text, sizeof(text))) {
        size_t n = strlen(text);
        if (n == 0 || n >= sizeof(text)) __builtin_trap();
        for (size_t i = 0; i < n; i++) {
            if (text[i] < 0x20 || text[i] > 0x7E) __builtin_trap();  // promised printable
        }
    }
    dhcpRequestOption(f.data(), f.size(), 224);
    return 0;
}
