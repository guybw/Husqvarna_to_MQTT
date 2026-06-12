// SPDX-License-Identifier: GPL-3.0-or-later
#include "status_led.h"
#include "config.h"

#include "driver/gpio.h"
#include "esp_timer.h"      // esp_timer_get_time → millis() replacement

namespace status_led {

    static Pattern  _pattern    = OFF;
    static uint32_t _last_tick  = 0;
    static bool     _led_on     = false;
    // BURST state: cycle of 3 quick flashes then a 2 s pause.
    static uint8_t  _burst_step = 0;

    // PORT: millis() → esp_timer_get_time() (microseconds) / 1000.
    static inline uint32_t now_ms() {
        return (uint32_t)(esp_timer_get_time() / 1000);
    }

    void begin() {
        // PORT: pinMode(OUTPUT) + digitalWrite(LOW) → gpio_config + set_level.
        gpio_config_t io = {};
        io.pin_bit_mask = (1ULL << PIN_STATUS_LED);
        io.mode         = GPIO_MODE_OUTPUT;
        io.pull_up_en   = GPIO_PULLUP_DISABLE;
        io.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io.intr_type    = GPIO_INTR_DISABLE;
        gpio_config(&io);
        gpio_set_level((gpio_num_t)PIN_STATUS_LED, 0); // LOW
    }

    void set(Pattern p) {
        _pattern    = p;
        _last_tick  = 0;
        _burst_step = 0;
        _led_on     = false;
    }

    void loop() {
        uint32_t now = now_ms();

        switch (_pattern) {
            case OFF:
                _led_on = false;
                break;

            case SOLID:
                _led_on = true;
                break;

            case BLINK_SLOW: // 1 Hz: 500 ms on / 500 ms off
                if (now - _last_tick >= 500) {
                    _last_tick = now;
                    _led_on = !_led_on;
                }
                break;

            case BLINK_FAST: // 5 Hz: 100 ms on / 100 ms off
                if (now - _last_tick >= 100) {
                    _last_tick = now;
                    _led_on = !_led_on;
                }
                break;

            case BURST: {
                // Steps 0-5: 3 × (50 ms on, 50 ms off); step 6: 2 s off.
                uint32_t interval = (_burst_step < 6) ? 50 : 2000;
                if (now - _last_tick >= interval) {
                    _last_tick = now;
                    _burst_step = (_burst_step >= 6) ? 0 : _burst_step + 1;
                    _led_on = (_burst_step % 2 == 0) && (_burst_step < 6);
                }
                break;
            }
        }

        // PORT: digitalWrite(HIGH/LOW) → gpio_set_level(1/0).
        gpio_set_level((gpio_num_t)PIN_STATUS_LED, _led_on ? 1 : 0);
    }
}
