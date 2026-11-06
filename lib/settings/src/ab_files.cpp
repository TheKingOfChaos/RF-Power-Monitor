#include "ab_files.h"

#include <LittleFS.h>

bool abReadFile(const char* path, char* buf, size_t size, bool& tooBig) {
    tooBig = false;
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    size_t len = f.readBytes(buf, size - 1);
    tooBig = f.available() > 0;
    f.close();
    buf[len] = '\0';
    return true;
}

bool abWriteFile(const char* what, const char* path, const char* data, size_t len) {
    File f = LittleFS.open(path, "w");
    if (!f) {
        Serial.printf("%s %s: cannot open for writing\r\n", what, path);
        return false;
    }
    size_t written = f.write((const uint8_t*)data, len);
    f.close();
    if (written != len) {
        Serial.printf("%s %s: short write (%u of %u bytes)\r\n", what, path, (unsigned)written, (unsigned)len);
        return false;
    }
    return true;
}
