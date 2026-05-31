// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings.h"
#include "config.h"
#include <Preferences.h>
#include <mbedtls/sha256.h>
#include <Arduino.h>

namespace settings {

    static Preferences prefs;

    // SHA-256 of an arbitrary string, returned as 64-char lowercase hex.
    static String sha256_hex(const String& input) {
        uint8_t hash[32];
        mbedtls_sha256(
            reinterpret_cast<const unsigned char*>(input.c_str()),
            input.length(), hash, 0 /* is224=0 → SHA-256 */);
        char hex[65];
        for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", hash[i]);
        hex[64] = '\0';
        return String(hex);
    }

    void begin() {
        prefs.begin("flymo", false);
    }

    // --- WiFi ---

    String get_wifi_ssid() { return prefs.getString("wifi.ssid", ""); }
    String get_wifi_psk()  { return prefs.getString("wifi.psk",  ""); }

    void set_wifi_ssid(const String& v) { prefs.putString("wifi.ssid", v); }
    void set_wifi_psk(const String& v)  { prefs.putString("wifi.psk",  v); }

    void clear_wifi() {
        prefs.remove("wifi.ssid");
        prefs.remove("wifi.psk");
    }

    void factory_reset() {
        prefs.clear(); // wipe entire "flymo" NVS namespace
    }

    // --- Admin credentials ---

    String get_admin_user() {
        return prefs.getString("admin.user", DEFAULT_ADMIN_USER);
    }

    void set_admin_user(const String& v) {
        prefs.putString("admin.user", v);
    }

    void set_admin_pass(const String& plaintext) {
        // 8 random bytes as 16-char hex salt
        char salt[17];
        for (int i = 0; i < 8; i++)
            snprintf(salt + i * 2, 3, "%02x", (uint8_t)(esp_random() & 0xFF));
        salt[16] = '\0';
        prefs.putString("admin.salt",   salt);
        prefs.putString("admin.pwhash", sha256_hex(String(salt) + plaintext));
    }

    bool verify_admin(const String& user, const String& pass) {
        if (user != get_admin_user()) return false;
        String salt = prefs.getString("admin.salt", "");
        if (salt.isEmpty()) {
            // No hash stored yet — accept the compile-time default password.
            return pass == DEFAULT_ADMIN_PASS;
        }
        return sha256_hex(salt + pass) == prefs.getString("admin.pwhash", "");
    }

    // --- Mower pairing ---

    String get_mower_mac() { return prefs.getString("mower.mac", ""); }
    void   set_mower_mac(const String& mac) { prefs.putString("mower.mac", mac); }

    uint32_t get_mower_pin() { return (uint32_t)prefs.getUInt("mower.pin", 1234); }
    void     set_mower_pin(uint32_t pin) { prefs.putUInt("mower.pin", pin); }

    uint32_t get_channel_id() { return prefs.getUInt("mower.chid", 0); }
    void     set_channel_id(uint32_t id) { prefs.putUInt("mower.chid", id); }

    bool get_maintenance_mode() { return prefs.getBool("sys.maint", false); }
    void set_maintenance_mode(bool on) { prefs.putBool("sys.maint", on); }

    // HTTP Basic Auth gate. Default OFF — enabling a wrong/forgotten password
    // on a headless device would lock the web UI out; BOOT-hold NVS wipe is
    // the physical recovery. Opt-in only, after bench-verifying login works.
    bool get_auth_enabled() { return prefs.getBool("sys.auth", false); }
    void set_auth_enabled(bool on) { prefs.putBool("sys.auth", on); }

    // Post-OTA reboot guard: 0=normal, 1=just OTA'd (force one clean reboot
    // before BLE init), 2=that clean reboot in progress. See main.cpp setup().
    uint8_t get_post_ota() { return prefs.getUChar("sys.postota", 0); }
    void    set_post_ota(uint8_t v) { prefs.putUChar("sys.postota", v); }

    uint32_t get_mow_override_secs() { return prefs.getUInt("mower.mowsec", 10800); }
    void     set_mow_override_secs(uint32_t s) { prefs.putUInt("mower.mowsec", s); }

    String   get_mqtt_host() { return prefs.getString("mqtt.host", ""); }
    void     set_mqtt_host(const String& v) { prefs.putString("mqtt.host", v); }
    uint16_t get_mqtt_port() { return (uint16_t)prefs.getUShort("mqtt.port", 1883); }
    void     set_mqtt_port(uint16_t p) { prefs.putUShort("mqtt.port", p); }
    String   get_mqtt_user() { return prefs.getString("mqtt.user", ""); }
    void     set_mqtt_user(const String& v) { prefs.putString("mqtt.user", v); }
    String   get_mqtt_pass() { return prefs.getString("mqtt.pass", ""); }
    void     set_mqtt_pass(const String& v) { prefs.putString("mqtt.pass", v); }
}
