// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <cstdint>

// NVS-backed settings. Namespace "flymo", keys lowercase+dotted (≤15 chars).
//
//   wifi.ssid      wifi.psk
//   admin.user     admin.pwhash    admin.salt
//   mower.mac      mower.pin       mower.name
//   mqtt.host      mqtt.port       mqtt.user      mqtt.pass

namespace settings {
    void begin();

    // WiFi
    std::string get_wifi_ssid();
    std::string get_wifi_psk();
    void   set_wifi_ssid(const std::string& v);
    void   set_wifi_psk(const std::string& v);
    void   clear_wifi();
    void   factory_reset(); // wipe all NVS keys → reboot to AP

    // Admin credentials (password stored as SHA-256(salt + plaintext))
    std::string get_admin_user();
    void   set_admin_user(const std::string& v);
    void   set_admin_pass(const std::string& plaintext); // generates fresh salt
    bool   verify_admin(const std::string& user, const std::string& pass);

    // Mower pairing — stored as NVS keys mower.mac / mower.pin / mower.chid
    std::string get_mower_mac();          // "aa:bb:cc:dd:ee:ff" or empty
    void     set_mower_mac(const std::string& mac);
    uint32_t get_mower_pin();          // numeric PIN (default 1234)
    void     set_mower_pin(uint32_t pin);
    uint32_t get_channel_id();         // persisted BLE channel ID (0 = not set)
    void     set_channel_id(uint32_t id);

    // Maintenance mode — when true, BLE is not started on boot (for OTA flashing)
    bool get_maintenance_mode();
    void set_maintenance_mode(bool on);
    // HTTP Basic Auth gate (default false; opt-in after bench-verifying login)
    bool get_auth_enabled();
    void set_auth_enabled(bool on);
    // Post-OTA clean-reboot guard (0 normal / 1 just-OTA'd / 2 forced reboot)
    uint8_t get_post_ota();
    void    set_post_ota(uint8_t v);

    // Mow override duration in seconds (SetOverrideMow). Default 10800 (3 h).
    uint32_t get_mow_override_secs();
    void     set_mow_override_secs(uint32_t s);

    // Idle re-check interval in minutes — how often the bridge wakes a resting
    // mower to read status/battery temp. Default 60. See ble_manager cadence.
    uint32_t get_idle_recheck_min();
    void     set_idle_recheck_min(uint32_t m);

    // MQTT broker. Empty host = MQTT disabled.
    std::string get_mqtt_host();
    void     set_mqtt_host(const std::string& v);
    uint16_t get_mqtt_port();          // default 1883
    void     set_mqtt_port(uint16_t p);
    std::string get_mqtt_user();
    void     set_mqtt_user(const std::string& v);
    std::string get_mqtt_pass();
    void     set_mqtt_pass(const std::string& v);
}
