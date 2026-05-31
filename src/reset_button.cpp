// SPDX-License-Identifier: GPL-3.0-or-later
#include "reset_button.h"
#include "config.h"
#include "settings.h"
#include "debug_log.h"
#include <Arduino.h>

namespace reset_button {

    void begin() {
        pinMode(PIN_RESET_BUTTON, INPUT_PULLUP);
    }

    void loop() {
        static uint32_t pressed_at = 0;

        if (digitalRead(PIN_RESET_BUTTON) == LOW) {
            if (pressed_at == 0) {
                pressed_at = millis();
            } else if (millis() - pressed_at >= RESET_HOLD_MS) {
                debug_log::write(debug_log::WARN, "reset",
                    "GPIO0 held %u ms — clearing WiFi credentials and restarting",
                    RESET_HOLD_MS);
                settings::clear_wifi();
                delay(200);
                ESP.restart();
            }
        } else {
            pressed_at = 0;
        }
    }
}
