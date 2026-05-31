// SPDX-License-Identifier: GPL-3.0-or-later
#include "automower_commands.h"

namespace automower_cmd {
    bool enter_operator_pin(uint32_t)  { return false; }
    bool get_state(uint32_t*)          { return false; }
    bool get_battery_level(uint8_t*)   { return false; }
    // TODO M5/M6
}
