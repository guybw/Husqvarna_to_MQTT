// SPDX-License-Identifier: GPL-3.0-or-later
//
// Flymo/Husqvarna BLE -> ESP32 -> MQTT bridge.
// M4: BLE scan (NimBLE) + SSE debug log stream.

#include <Arduino.h>
#include "config.h"
#include "settings.h"
#include "debug_log.h"
#include "status_led.h"
#include "reset_button.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "ble/ble_manager.h"
#include "mqtt.h"

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println();

    settings::begin();
    debug_log::begin();
    status_led::begin();
    reset_button::begin();

    debug_log::write(debug_log::INFO, "main", "booting  fw=%s", FIRMWARE_VERSION);

    // Why did we last reset? brownout/panic/power-on — key for the
    // intermittent "won't come up" issue.
    {
        esp_reset_reason_t rr = esp_reset_reason();
        const char* s =
            rr == ESP_RST_POWERON   ? "POWERON"   :
            rr == ESP_RST_BROWNOUT  ? "BROWNOUT (power!)" :
            rr == ESP_RST_PANIC     ? "PANIC (crash)" :
            rr == ESP_RST_INT_WDT   ? "INT_WDT"   :
            rr == ESP_RST_TASK_WDT  ? "TASK_WDT"  :
            rr == ESP_RST_WDT       ? "WDT"       :
            rr == ESP_RST_SW        ? "SW (restart)" :
            rr == ESP_RST_DEEPSLEEP ? "DEEPSLEEP" :
            rr == ESP_RST_EXT       ? "EXT"       : "OTHER";
        debug_log::write(debug_log::INFO, "main", "reset reason: %s", s);
    }

    // Post-OTA crash guard. NimBLE init PANICs on the first boot after an
    // OTA soft-reset (confirmed: reset reason PANIC), but a subsequent clean
    // boot is fine. So after an OTA force exactly one extra clean reboot
    // before anything (esp. BLE) initialises: 1 → 2 → restart → 2 → clear.
    {
        uint8_t po = settings::get_post_ota();
        if (po == 1) {
            settings::set_post_ota(2);
            debug_log::write(debug_log::WARN, "main",
                "post-OTA: forcing one clean reboot before BLE init");
            delay(200);
            ESP.restart();
        } else if (po == 2) {
            settings::set_post_ota(0);
            debug_log::write(debug_log::INFO, "main",
                "post-OTA clean boot — BLE safe to init");
        }
    }

    wifi_manager::begin();
    web_server::begin();
    if (settings::get_maintenance_mode()) {
        debug_log::write(debug_log::WARN, "main",
            "MAINTENANCE MODE — BLE disabled. Use web UI to exit.");
    } else {
        ble_manager::begin();
    }
    mqtt::begin();
}

void loop() {
    reset_button::loop();
    wifi_manager::loop();
    status_led::loop();
    web_server::loop();
    ble_manager::loop();
    mqtt::loop();

    static uint32_t last_hb = 0;
    uint32_t now = millis();
    if (now - last_hb >= 10000) {
        last_hb = now;
        debug_log::write(debug_log::DEBUG, "main",
            "uptime=%lus  heap=%u  wifi=%s",
            now / 1000,
            (unsigned)ESP.getFreeHeap(),
            wifi_manager::is_connected()   ? "STA" :
            wifi_manager::in_portal_mode() ? "AP"  : "none");
    }
}
