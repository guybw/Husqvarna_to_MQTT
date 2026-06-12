// SPDX-License-Identifier: GPL-3.0-or-later
#include "crc8_maxim.h"

namespace crc8 {
    // CRC-8/MAXIM-DOW (a.k.a. CRC-8/1-Wire): poly 0x31, init 0x00, refin/refout.
    // Reflected polynomial 0x8C used in the table (reflect(0x31, 8) = 0x8C).
    static uint8_t _table[256];
    static bool    _ready = false;

    static void build_table() {
        for (int i = 0; i < 256; i++) {
            uint8_t b = (uint8_t)i;
            for (int j = 0; j < 8; j++)
                b = (b & 1) ? ((b >> 1) ^ 0x8C) : (b >> 1);
            _table[i] = b;
        }
        _ready = true;
    }

    uint8_t maxim(const uint8_t* data, size_t len) {
        if (!_ready) build_table();
        uint8_t crc = 0x00;
        while (len--) crc = _table[crc ^ *data++];
        return crc;
    }
}
