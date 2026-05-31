// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// STA-first WiFi bring-up with captive-portal fallback. mDNS.
// Implemented in M1.

namespace wifi_manager {
    void begin();
    void loop();
    bool is_connected();
    bool in_portal_mode();
}
