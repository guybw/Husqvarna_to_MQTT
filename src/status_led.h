// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Non-blocking LED state machine.
//   OFF         boot / no WiFi
//   BLINK_SLOW  AP / captive portal (1 Hz)
//   BLINK_FAST  connecting (WiFi or BLE, 5 Hz)
//   SOLID       all good
//   BURST       recent BLE error (auto-clears after 10 s)
//
// Implemented in M2.

namespace status_led {
    enum Pattern { OFF, BLINK_SLOW, BLINK_FAST, SOLID, BURST };

    void begin();
    void set(Pattern p);
    void loop();
}
