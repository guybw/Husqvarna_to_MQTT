// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stddef.h>
#include <stdint.h>

// CRC-8/MAXIM-DOW (a.k.a. CRC-8/DALLAS, polynomial 0x31, init 0x00, no reflect).
// Used in the Automower BLE frame format -- both the 9-byte header CRC and the
// payload CRC. Lookup-table implementation lifted from the Marbanz fork's
// helpers.py. Filled in during M5.

namespace crc8 {
    uint8_t maxim(const uint8_t* data, size_t len);
}
