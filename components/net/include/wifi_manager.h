// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// STA-first WiFi bring-up with captive-portal fallback. mDNS.
// Ported to native ESP-IDF (esp_wifi event model + mdns component +
// a minimal captive-DNS task). Public API unchanged from the Arduino version.

namespace wifi_manager {
    void begin();
    void loop();
    bool is_connected();
    bool in_portal_mode();
}
