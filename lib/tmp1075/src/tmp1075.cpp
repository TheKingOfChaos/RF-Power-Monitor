#include "tmp1075.h"

int32_t tmp1075ToDeciC(uint16_t raw) {
    int32_t scaled = (int32_t)(int16_t)raw * 10;  // tenths of a degree, times 256
    return scaled >= 0 ? (scaled + 128) / 256 : -((-scaled + 128) / 256);
}

void TempSensorState::reading(bool ok, int32_t deciC) {
    if (ok) {
        _valid = true;
        _deciC = deciC;
        _failuresInRow = 0;
        return;
    }
    _readErrors++;
    if (_failuresInRow < MAX_FAILURES) _failuresInRow++;
    if (_failuresInRow >= MAX_FAILURES) _valid = false;
}
