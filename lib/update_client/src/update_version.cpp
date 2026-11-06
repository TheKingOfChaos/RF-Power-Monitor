#include "update_version.h"

#include <string.h>

namespace {

uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/// "M.m.p" or "M.m.p-pre": numbers in n, the pre-release part (or "") in pre
bool splitVersion(const char* v, uint32_t n[3], const char*& pre) {
    const char* p = v;
    for (int i = 0; i < 3; i++) {
        if (*p < '0' || *p > '9') return false;
        uint32_t x = 0;
        while (*p >= '0' && *p <= '9') {
            if (x > 100000) return false;
            x = x * 10 + (uint32_t)(*p++ - '0');
        }
        n[i] = x;
        if (i < 2 && *p++ != '.') return false;
    }
    if (*p == '\0') {
        pre = "";
        return true;
    }
    if (*p != '-' || p[1] == '\0') return false;
    pre = p + 1;
    return true;
}

}  // namespace

bool updHeaderVersion(const uint8_t* head, size_t len, char out[UPDATE_VERSION_MAX + 1]) {
    out[0] = '\0';
    if (head == nullptr || len < UPD_HEAD_SIZE) return false;
    if (le32(head) != 0x50554652u || le32(head + 4) != 1) return false;
    const char* v = (const char*)head + 8;
    size_t n = strnlen(v, 16);
    if (n == 0 || n == 16) return false;
    memcpy(out, v, n);
    out[n] = '\0';
    return true;
}

int compareVersions(const char* a, const char* b) {
    uint32_t na[3], nb[3];
    const char *pa, *pb;
    if (!splitVersion(a, na, pa) || !splitVersion(b, nb, pb)) return strcmp(a, b);
    for (int i = 0; i < 3; i++) {
        if (na[i] != nb[i]) return na[i] < nb[i] ? -1 : 1;
    }
    if (*pa == '\0' || *pb == '\0') return (*pa == '\0') - (*pb == '\0');  // release ranks above pre-release
    return strcmp(pa, pb);
}
