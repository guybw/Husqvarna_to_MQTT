// SPDX-License-Identifier: GPL-3.0-or-later
//
// Flymo/Husqvarna BLE -> ESP32 -> MQTT bridge.
// Native ESP-IDF port — entry point (app_main replaces Arduino setup()/loop()).

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "config.h"
#include "settings.h"
#include "debug_log.h"
#include "status_led.h"
#include "reset_button.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "ble_manager.h"
#include "mqtt.h"

static const char* TAG = "main";

// Fast tick for the cooperative module loops. status_led blinking and
// reset_button long-press detection need frequent service, so the heartbeat
// log is throttled to 10 s on top of this cadence.
static constexpr uint32_t LOOP_TICK_MS = 20;
static constexpr uint32_t HEARTBEAT_MS = 10000;

extern "C" void app_main(void)
{
    // NVS is needed by WiFi, settings, and the NimBLE bond store.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    settings::begin();
    debug_log::begin();
    status_led::begin();
    reset_button::begin();

    debug_log::write(debug_log::INFO, "main", "booting  fw=%s", FIRMWARE_VERSION);
    ESP_LOGI(TAG, "flymo-bridge booting  fw=%s", FIRMWARE_VERSION);

    // Why did we last reset? brownout/panic/power-on — key for the
    // intermittent "won't come up" issue.
    esp_reset_reason_t rr = esp_reset_reason();
    const char* s =
        rr == ESP_RST_POWERON   ? "POWERON" :
        rr == ESP_RST_BROWNOUT  ? "BROWNOUT (power!)" :
        rr == ESP_RST_PANIC     ? "PANIC (crash)" :
        rr == ESP_RST_INT_WDT   ? "INT_WDT" :
        rr == ESP_RST_TASK_WDT  ? "TASK_WDT" :
        rr == ESP_RST_SW        ? "SW (restart)" :
        rr == ESP_RST_DEEPSLEEP ? "DEEPSLEEP" :
        rr == ESP_RST_EXT       ? "EXT" : "OTHER";
    debug_log::write(debug_log::INFO, "main", "reset reason: %s", s);

    // Post-OTA crash guard. NimBLE init PANICs on the first boot after an OTA
    // soft-reset (confirmed in the field), but a subsequent clean boot is fine.
    // So after an OTA force exactly one extra clean reboot before BLE init.
    {
        uint8_t po = settings::get_post_ota();
        if (po == 1) {
            settings::set_post_ota(2);
            debug_log::write(debug_log::WARN, "main",
                "post-OTA: forcing one clean reboot before BLE init");
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
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

    ESP_LOGI(TAG, "init complete. free heap=%u", (unsigned)esp_get_free_heap_size());

    uint32_t since_hb = 0;
    while (true) {
        reset_button::loop();
        wifi_manager::loop();
        status_led::loop();
        web_server::loop();
        ble_manager::loop();
        mqtt::loop();

        vTaskDelay(pdMS_TO_TICKS(LOOP_TICK_MS));

        since_hb += LOOP_TICK_MS;
        if (since_hb >= HEARTBEAT_MS) {
            since_hb = 0;
            debug_log::write(debug_log::DEBUG, "main",
                "uptime=%lus  heap=%u  wifi=%s",
                (unsigned long)(esp_timer_get_time() / 1000000),
                (unsigned)esp_get_free_heap_size(),
                wifi_manager::is_connected()   ? "STA" :
                wifi_manager::in_portal_mode() ? "AP"  : "none");
        }
    }
}
