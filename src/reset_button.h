// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// GPIO0 long-press (>RESET_HOLD_MS) -> nvs_flash_erase() + ESP.restart().
// Single physical recovery path once the device is deployed.
// Implemented in M2.

namespace reset_button {
    void begin();
    void loop();
}
