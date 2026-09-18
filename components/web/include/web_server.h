// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>  // size_t

// All HTTP routes: / /setup /pair /debug /update + /api/*.
// HTTP Basic Auth (opt-in via settings::get_auth_enabled).
// OTA via esp_ota. SSE stream of the debug ring buffer.
// Native ESP-IDF port (esp_http_server). Public API unchanged from the
// Arduino version.

namespace web_server {
    void begin();
    void loop();   // no-op under ESP-IDF (SSE drains in its own blocking
                   // handler; ArduinoOTA removed). Kept for API compatibility.

    // GitHub-hosted OTA (docs/ on main — the same files the web flasher uses).
    // Blocking HTTPS GET of docs/version.txt (a few hundred ms - ~2 s); safe to
    // call from any task except the esp-mqtt event callback. Copies the trimmed
    // version string into out (out_len incl. NUL) and returns true on success.
    // out_status (optional) gets the HTTP status code (0 if the request never
    // got a response at all, e.g. no network) — e.g. 404 means docs/version.txt
    // doesn't exist on the main branch (nothing pushed yet), not a connectivity
    // problem.
    bool check_github_version(char* out, size_t out_len, int* out_status = nullptr);

    // Kicks off the fetch-flash-reboot of docs/firmware.bin from GitHub on a
    // dedicated one-shot task; returns immediately. Suspends BLE like the
    // manual upload path — one-way, the device reboots on success. On failure
    // it logs and leaves BLE suspended (same tradeoff as a failed manual OTA
    // upload today), no reboot.
    void start_github_ota_async();
}
