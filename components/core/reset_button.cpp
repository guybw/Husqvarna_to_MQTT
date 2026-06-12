// SPDX-License-Identifier: GPL-3.0-or-later
#include "reset_button.h"
#include "config.h"
#include "settings.h"
#include "debug_log.h"

#include "driver/gpio.h"
#include "esp_timer.h"      // esp_timer_get_time → millis() replacement
#include "esp_system.h"     // esp_restart
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"  // vTaskDelay

namespace reset_button {

    // PORT: millis() → esp_timer_get_time() (microseconds) / 1000.
    static inline uint32_t now_ms() {
        return (uint32_t)(esp_timer_get_time() / 1000);
    }

    void begin() {
        // PORT: pinMode(INPUT_PULLUP) → gpio_config with internal pull-up.
        gpio_config_t io = {};
        io.pin_bit_mask = (1ULL << PIN_RESET_BUTTON);
        io.mode         = GPIO_MODE_INPUT;
        io.pull_up_en   = GPIO_PULLUP_ENABLE;
        io.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io.intr_type    = GPIO_INTR_DISABLE;
        gpio_config(&io);
    }

    void loop() {
        static uint32_t pressed_at = 0;

        // PORT: digitalRead(...) == LOW → gpio_get_level(...) == 0.
        if (gpio_get_level((gpio_num_t)PIN_RESET_BUTTON) == 0) {
            if (pressed_at == 0) {
                pressed_at = now_ms();
            } else if (now_ms() - pressed_at >= RESET_HOLD_MS) {
                debug_log::write(debug_log::WARN, "reset",
                    "GPIO0 held %u ms — clearing WiFi credentials and restarting",
                    RESET_HOLD_MS);
                // PORT: preserve the reference behaviour — the Arduino impl
                // calls clear_wifi() + ESP.restart() (NOT factory_reset()),
                // despite the header comment / spec mentioning factory_reset.
                // Kept faithful so a long-press only drops WiFi creds, not the
                // mower bond/pairing. Review if a full wipe is actually wanted.
                settings::clear_wifi();
                vTaskDelay(pdMS_TO_TICKS(200)); // delay(200)
                esp_restart();                  // ESP.restart()
            }
        } else {
            pressed_at = 0;
        }
    }
}
