// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings.h"
#include "config.h"

#include <cstdio>
#include <cstring>

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_system.h"      // esp_random, esp_restart
#include "esp_random.h"
#include "mbedtls/md.h"

namespace settings {

    static const char* NVS_NS = "flymo";

    // --- NVS handle helpers ---------------------------------------------------
    // PORT: Arduino's Preferences kept a single open handle for the namespace.
    // ESP-IDF's nvs_open is cheap, so we open a fresh handle per access and
    // close it. This keeps each getter/setter self-contained and avoids holding
    // a global handle across the lifetime of the app.

    static nvs_handle_t open_rw() {
        nvs_handle_t h = 0;
        // Best-effort; on failure h stays 0 and callers fall back to defaults.
        nvs_open(NVS_NS, NVS_READWRITE, &h);
        return h;
    }

    // Read a string key. Returns def if missing/empty. Uses the two-call pattern.
    static std::string nvs_get_string(const char* key, const std::string& def) {
        nvs_handle_t h = 0;
        if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return def;
        size_t len = 0;
        esp_err_t err = nvs_get_str(h, key, nullptr, &len);
        if (err != ESP_OK || len == 0) {
            nvs_close(h);
            return def;
        }
        std::string out;
        out.resize(len); // includes the NUL terminator
        err = nvs_get_str(h, key, &out[0], &len);
        nvs_close(h);
        if (err != ESP_OK) return def;
        if (!out.empty() && out.back() == '\0') out.pop_back(); // drop NUL
        return out;
    }

    static void nvs_set_string(const char* key, const std::string& v) {
        nvs_handle_t h = open_rw();
        if (!h) return;
        nvs_set_str(h, key, v.c_str());
        nvs_commit(h);
        nvs_close(h);
    }

    static uint32_t nvs_get_u32(const char* key, uint32_t def) {
        nvs_handle_t h = 0;
        if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return def;
        uint32_t v = def;
        // ::-qualify: this static helper shares the name of the real NVS API.
        if (::nvs_get_u32(h, key, &v) != ESP_OK) v = def;
        nvs_close(h);
        return v;
    }

    static void nvs_set_u32(const char* key, uint32_t v) {
        nvs_handle_t h = open_rw();
        if (!h) return;
        ::nvs_set_u32(h, key, v);
        nvs_commit(h);
        nvs_close(h);
    }

    static uint8_t nvs_get_u8(const char* key, uint8_t def) {
        nvs_handle_t h = 0;
        if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return def;
        uint8_t v = def;
        if (::nvs_get_u8(h, key, &v) != ESP_OK) v = def;
        nvs_close(h);
        return v;
    }

    static void nvs_set_u8(const char* key, uint8_t v) {
        nvs_handle_t h = open_rw();
        if (!h) return;
        ::nvs_set_u8(h, key, v);
        nvs_commit(h);
        nvs_close(h);
    }

    static void nvs_remove(const char* key) {
        nvs_handle_t h = open_rw();
        if (!h) return;
        nvs_erase_key(h, key); // ESP_ERR_NVS_NOT_FOUND is harmless
        nvs_commit(h);
        nvs_close(h);
    }

    // SHA-256 of an arbitrary string, returned as 64-char lowercase hex.
    static std::string sha256_hex(const std::string& input) {
        uint8_t hash[32];
        // IDF 6.2 ships mbedtls 4.x, where <mbedtls/sha256.h> is no longer on the
        // public include path. Use the version-stable generic message-digest API
        // (<mbedtls/md.h>) instead. The SHA-256 output bytes are identical, so
        // admin password hashes stored by the Arduino firmware remain valid.
        mbedtls_md(
            mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
            reinterpret_cast<const unsigned char*>(input.c_str()),
            input.length(), hash);
        char hex[65];
        for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", hash[i]);
        hex[64] = '\0';
        return std::string(hex);
    }

    void begin() {
        // PORT: nvs_flash_init() is normally done once in app_main(); we call it
        // here defensively so settings work even if begin() runs first. It is
        // idempotent — a second init returns ESP_OK without side effects.
        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            nvs_flash_erase();
            nvs_flash_init();
        }
    }

    // --- WiFi ---

    std::string get_wifi_ssid() { return nvs_get_string("wifi.ssid", ""); }
    std::string get_wifi_psk()  { return nvs_get_string("wifi.psk",  ""); }

    void set_wifi_ssid(const std::string& v) { nvs_set_string("wifi.ssid", v); }
    void set_wifi_psk(const std::string& v)  { nvs_set_string("wifi.psk",  v); }

    void clear_wifi() {
        nvs_remove("wifi.ssid");
        nvs_remove("wifi.psk");
    }

    void factory_reset() {
        // wipe entire "flymo" NVS namespace, then reboot
        nvs_handle_t h = open_rw();
        if (h) {
            nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
        }
        // PORT: original Preferences::clear() did not reboot here; but the API
        // contract (see settings.h: "wipe all NVS keys → reboot to AP") and the
        // task spec require esp_restart() after a factory reset.
        esp_restart();
    }

    // --- Admin credentials ---

    std::string get_admin_user() {
        return nvs_get_string("admin.user", DEFAULT_ADMIN_USER);
    }

    void set_admin_user(const std::string& v) {
        nvs_set_string("admin.user", v);
    }

    void set_admin_pass(const std::string& plaintext) {
        // 8 random bytes as 16-char hex salt
        char salt[17];
        for (int i = 0; i < 8; i++)
            snprintf(salt + i * 2, 3, "%02x", (uint8_t)(esp_random() & 0xFF));
        salt[16] = '\0';
        nvs_set_string("admin.salt",   std::string(salt));
        nvs_set_string("admin.pwhash", sha256_hex(std::string(salt) + plaintext));
    }

    bool verify_admin(const std::string& user, const std::string& pass) {
        if (user != get_admin_user()) return false;
        std::string salt = nvs_get_string("admin.salt", "");
        if (salt.empty()) {
            // No hash stored yet — accept the compile-time default password.
            return pass == DEFAULT_ADMIN_PASS;
        }
        return sha256_hex(salt + pass) == nvs_get_string("admin.pwhash", "");
    }

    // --- Mower pairing ---

    std::string get_mower_mac() { return nvs_get_string("mower.mac", ""); }
    void   set_mower_mac(const std::string& mac) { nvs_set_string("mower.mac", mac); }

    uint32_t get_mower_pin() { return nvs_get_u32("mower.pin", 1234); }
    void     set_mower_pin(uint32_t pin) { nvs_set_u32("mower.pin", pin); }

    uint32_t get_channel_id() { return nvs_get_u32("mower.chid", 0); }
    void     set_channel_id(uint32_t id) { nvs_set_u32("mower.chid", id); }

    bool get_maintenance_mode() { return nvs_get_u8("sys.maint", 0) != 0; }
    void set_maintenance_mode(bool on) { nvs_set_u8("sys.maint", on ? 1 : 0); }

    // HTTP Basic Auth gate. Default OFF — enabling a wrong/forgotten password
    // on a headless device would lock the web UI out; BOOT-hold NVS wipe is
    // the physical recovery. Opt-in only, after bench-verifying login works.
    bool get_auth_enabled() { return nvs_get_u8("sys.auth", 0) != 0; }
    void set_auth_enabled(bool on) { nvs_set_u8("sys.auth", on ? 1 : 0); }

    // Post-OTA reboot guard: 0=normal, 1=just OTA'd (force one clean reboot
    // before BLE init), 2=that clean reboot in progress. See main.cpp setup().
    uint8_t get_post_ota() { return nvs_get_u8("sys.postota", 0); }
    void    set_post_ota(uint8_t v) { nvs_set_u8("sys.postota", v); }

    uint32_t get_mow_override_secs() { return nvs_get_u32("mower.mowsec", 10800); }
    void     set_mow_override_secs(uint32_t s) { nvs_set_u32("mower.mowsec", s); }

    uint32_t get_idle_recheck_min() { return nvs_get_u32("ble.recheck", 60); }
    void     set_idle_recheck_min(uint32_t m) { nvs_set_u32("ble.recheck", m); }

    std::string get_mqtt_host() { return nvs_get_string("mqtt.host", ""); }
    void     set_mqtt_host(const std::string& v) { nvs_set_string("mqtt.host", v); }
    // Stored as NVS u16 to match the Arduino reference's Preferences::putUShort
    // (NVS is strongly typed — a u16 key cannot be read back via nvs_get_u32),
    // so a port saved by the old firmware survives the upgrade.
    uint16_t get_mqtt_port() {
        nvs_handle_t h = 0;
        if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return 1883;
        uint16_t v = 1883;
        if (nvs_get_u16(h, "mqtt.port", &v) != ESP_OK) v = 1883;
        nvs_close(h);
        return v;
    }
    void set_mqtt_port(uint16_t p) {
        nvs_handle_t h = open_rw();
        if (!h) return;
        nvs_set_u16(h, "mqtt.port", p);
        nvs_commit(h);
        nvs_close(h);
    }
    std::string get_mqtt_user() { return nvs_get_string("mqtt.user", ""); }
    void     set_mqtt_user(const std::string& v) { nvs_set_string("mqtt.user", v); }
    std::string get_mqtt_pass() { return nvs_get_string("mqtt.pass", ""); }
    void     set_mqtt_pass(const std::string& v) { nvs_set_string("mqtt.pass", v); }
}
