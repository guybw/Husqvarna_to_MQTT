// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// All HTTP routes: / /setup /pair /debug /update + /api/*.
// HTTP Basic Auth (opt-in via settings::get_auth_enabled).
// OTA via esp_ota. SSE stream of the debug ring buffer.
// Native ESP-IDF port (esp_http_server). Public API unchanged from the
// Arduino version.

namespace web_server {
    void begin();
    void loop();   // no-op under ESP-IDF (SSE drains in its own blocking
                   // handler; ArduinoOTA removed). Kept for API compatibility.
}
