// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native ESP-IDF port of the Arduino web_server.cpp. Uses esp_http_server
// instead of ESPAsyncWebServer, esp_ota instead of Update.h, and a blocking
// SSE handler instead of AsyncEventSource. Every route + JSON shape is kept
// identical to the Arduino reference.

#include "web_server.h"
#include "config.h"
#include "settings.h"
#include "debug_log.h"
#include "wifi_manager.h"
#include "ble_manager.h"
#include "web_assets.h"   // gzipped UI embedded in flash (generated)

#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_mac.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <mbedtls/base64.h>
#include <ArduinoJson.h>

#include <string>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#define SRC "web"

#ifndef DEBUG_LOG_LINE_MAX
#define DEBUG_LOG_LINE_MAX 256
#endif

namespace web_server {

    static httpd_handle_t _server = nullptr;

    // ── SSE marshalling ──────────────────────────────────────────────────
    // The producer (debug_log::write, called from many tasks) only enqueues.
    // The single blocking SSE handler owns the socket and drains the queue.
    struct SseMsg {
        uint32_t id;
        char     json[DEBUG_LOG_LINE_MAX * 2 + 80];
    };
    static constexpr uint8_t  SSE_Q_DEPTH    = 12;
    static constexpr uint16_t SSE_REPLAY_MAX = 40;
    static QueueHandle_t _sse_q = nullptr;
    // Single admin SSE client. -1 = none. The producer checks this to know
    // whether anyone is listening before bothering to enqueue.
    static volatile int _sse_fd = -1;

    static inline int64_t millis() { return esp_timer_get_time() / 1000; }

    // ── helpers ──────────────────────────────────────────────────────────

    static void schedule_restart(uint32_t ms) {
        // One-shot esp_timer → esp_restart(). Falls back to immediate restart.
        esp_timer_create_args_t args = {};
        args.callback = [](void*) { esp_restart(); };
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "rst";
        esp_timer_handle_t t = nullptr;
        if (esp_timer_create(&args, &t) == ESP_OK) {
            esp_timer_start_once(t, (uint64_t)ms * 1000);
        } else {
            esp_restart();
        }
    }

    // Send a JSON (or plain) response with an explicit status line.
    static esp_err_t send_json(httpd_req_t* req, const char* status,
                               const char* body) {
        httpd_resp_set_status(req, status);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, body);
    }
    static esp_err_t send_ok(httpd_req_t* req, const char* body) {
        return send_json(req, "200 OK", body);
    }

    // HTTP Basic Auth gate. Returns true if the request may proceed. When auth
    // is disabled this is a no-op. On failure sends a 401 challenge and the
    // caller must return ESP_OK.
    static bool guard(httpd_req_t* req) {
        if (!settings::get_auth_enabled()) return true;
        char hdr[256];
        if (httpd_req_get_hdr_value_str(req, "Authorization", hdr,
                                        sizeof(hdr)) == ESP_OK) {
            if (strncmp(hdr, "Basic ", 6) == 0) {
                const char* b64 = hdr + 6;
                unsigned char out[160];
                size_t olen = 0;
                if (mbedtls_base64_decode(out, sizeof(out) - 1, &olen,
                        (const unsigned char*)b64, strlen(b64)) == 0) {
                    out[olen] = 0;
                    std::string creds = (char*)out;
                    size_t c = creds.find(':');
                    if (c != std::string::npos && c > 0 &&
                        settings::verify_admin(creds.substr(0, c),
                                               creds.substr(c + 1)))
                        return true;
                }
            }
        }
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"flymo\"");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Unauthorized");
        return false;
    }

    // URL-decode in place (%xx and '+'). Returns length.
    static size_t url_decode(char* s) {
        char* w = s;
        for (char* r = s; *r; ++r) {
            if (*r == '+') {
                *w++ = ' ';
            } else if (*r == '%' && r[1] && r[2]) {
                auto hex = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return -1;
                };
                int hi = hex(r[1]), lo = hex(r[2]);
                if (hi >= 0 && lo >= 0) {
                    *w++ = (char)((hi << 4) | lo);
                    r += 2;
                } else {
                    *w++ = *r;
                }
            } else {
                *w++ = *r;
            }
        }
        *w = '\0';
        return (size_t)(w - s);
    }

    // Read the full request body into a heap buffer (NUL-terminated). Caller
    // frees. Returns nullptr on error/empty. Caps at max_len.
    static char* read_body(httpd_req_t* req, size_t max_len, size_t* out_len) {
        size_t total = req->content_len;
        if (total == 0) return nullptr;
        if (total > max_len) total = max_len;
        char* buf = (char*)malloc(total + 1);
        if (!buf) return nullptr;
        size_t got = 0;
        while (got < total) {
            int r = httpd_req_recv(req, buf + got, total - got);
            if (r <= 0) {
                if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
                free(buf);
                return nullptr;
            }
            got += (size_t)r;
        }
        // Drain any remaining bytes beyond the cap so the socket stays sane.
        size_t remaining = req->content_len - got;
        char scratch[128];
        while (remaining > 0) {
            int r = httpd_req_recv(req, scratch,
                                   remaining < sizeof(scratch) ? remaining
                                                               : sizeof(scratch));
            if (r <= 0) break;
            remaining -= (size_t)r;
        }
        buf[got] = '\0';
        if (out_len) *out_len = got;
        return buf;
    }

    // Pull a key from an application/x-www-form-urlencoded body. URL-decodes
    // the value. Returns true if found.
    static bool form_get(const char* body, const char* key, char* out,
                         size_t outsz) {
        size_t klen = strlen(key);
        const char* p = body;
        while (p && *p) {
            const char* amp = strchr(p, '&');
            const char* eq = strchr(p, '=');
            const char* seg_end = amp ? amp : p + strlen(p);
            if (eq && eq < seg_end &&
                (size_t)(eq - p) == klen && strncmp(p, key, klen) == 0) {
                size_t vlen = (size_t)(seg_end - (eq + 1));
                if (vlen >= outsz) vlen = outsz - 1;
                memcpy(out, eq + 1, vlen);
                out[vlen] = '\0';
                url_decode(out);
                return true;
            }
            if (!amp) break;
            p = amp + 1;
        }
        return false;
    }

    // Format the current IP (STA if connected, else AP) into out.
    static void current_ip(char* out, size_t outsz) {
        const char* key = wifi_manager::is_connected() ? "WIFI_STA_DEF"
                                                       : "WIFI_AP_DEF";
        esp_netif_t* nif = esp_netif_get_handle_from_ifkey(key);
        esp_netif_ip_info_t ip = {};
        if (nif && esp_netif_get_ip_info(nif, &ip) == ESP_OK) {
            snprintf(out, outsz, IPSTR, IP2STR(&ip.ip));
        } else {
            snprintf(out, outsz, "0.0.0.0");
        }
    }

    // ── schedule JSON helpers (ported verbatim from the Arduino source) ──

    static bool parse_task_time(const char* text, uint32_t& out_secs) {
        if (!text || text[0] == '\0') return false;
        int h = -1, m = -1;
        if (sscanf(text, "%d:%d", &h, &m) == 2) {
            if (h < 0 || h > 23 || m < 0 || m > 59) return false;
            out_secs = (uint32_t)h * 3600 + (uint32_t)m * 60;
            return true;
        }
        char* end;
        long v = strtol(text, &end, 10);
        if (end && *end == '\0' && v >= 0) {
            out_secs = (uint32_t)v;
            return true;
        }
        return false;
    }

    static bool parse_task_day(JsonVariant v, int& out_index) {
        if (v.is<int>()) {
            int value = v.as<int>();
            if (value >= 0 && value <= 6) {
                out_index = value;
                return true;
            }
            return false;
        }
        const char* text = v.as<const char*>();
        if (!text) return false;
        std::string s(text);
        for (auto& ch : s) ch = (char)tolower((unsigned char)ch);
        if (s == "mon" || s == "monday") { out_index = 0; return true; }
        if (s == "tue" || s == "tuesday") { out_index = 1; return true; }
        if (s == "wed" || s == "wednesday") { out_index = 2; return true; }
        if (s == "thu" || s == "thursday") { out_index = 3; return true; }
        if (s == "fri" || s == "friday") { out_index = 4; return true; }
        if (s == "sat" || s == "saturday") { out_index = 5; return true; }
        if (s == "sun" || s == "sunday") { out_index = 6; return true; }
        return false;
    }

    static bool parse_task_days(JsonArray arr, bool out_days[7]) {
        for (int i = 0; i < 7; i++) out_days[i] = false;
        for (JsonVariant v : arr) {
            int idx = -1;
            if (!parse_task_day(v, idx) || idx < 0 || idx > 6) return false;
            out_days[idx] = true;
        }
        return true;
    }

    static void emit_task_json(JsonObject obj, uint32_t id,
                               const ble_manager::ScheduleTask& task) {
        obj["id"] = id;
        obj["start"] = task.start;
        obj["duration"] = task.duration;
        JsonArray days = obj.createNestedArray("days");
        static const char* names[7] = {
            "Monday", "Tuesday", "Wednesday", "Thursday",
            "Friday", "Saturday", "Sunday"
        };
        for (int i = 0; i < 7; i++) if (task.use_on[i]) days.add(names[i]);
    }

    // ── asset serving ────────────────────────────────────────────────────

    static esp_err_t send_gz(httpd_req_t* req, const char* ctype,
                             const uint8_t* body, size_t len) {
        httpd_resp_set_type(req, ctype);
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        return httpd_resp_send(req, (const char*)body, len);
    }

    static esp_err_t redirect(httpd_req_t* req, const char* location) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", location);
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, "", 0);
    }

    // ── SSE broadcast (producer side) ────────────────────────────────────
    // Called by debug_log on every new entry, from ANY task. Only enqueues.
    static void sse_broadcast(const char* json, uint32_t id) {
        if (!_sse_q || _sse_fd < 0) return;
        SseMsg m;
        m.id = id;
        strncpy(m.json, json, sizeof(m.json) - 1);
        m.json[sizeof(m.json) - 1] = '\0';
        xQueueSend(_sse_q, &m, 0); // non-blocking; drop on full
    }

    // Write one SSE frame to the response chunk stream. Used by replay + live.
    static esp_err_t sse_send_frame(httpd_req_t* req, const char* json,
                                    uint32_t id) {
        char hdr[48];
        int n = snprintf(hdr, sizeof(hdr), "id: %lu\nevent: log\ndata: ",
                         (unsigned long)id);
        if (httpd_resp_send_chunk(req, hdr, n) != ESP_OK) return ESP_FAIL;
        if (httpd_resp_send_chunk(req, json, strlen(json)) != ESP_OK)
            return ESP_FAIL;
        if (httpd_resp_send_chunk(req, "\n\n", 2) != ESP_OK) return ESP_FAIL;
        return ESP_OK;
    }

    // ── route handlers ───────────────────────────────────────────────────

    static esp_err_t h_root(httpd_req_t* req) {
        if (!guard(req)) return ESP_OK;
        return send_gz(req, "text/html", WEB_INDEX_HTML_GZ,
                       WEB_INDEX_HTML_GZ_LEN);
    }

    static esp_err_t h_style(httpd_req_t* req) {
        return send_gz(req, "text/css", WEB_STYLE_CSS_GZ, WEB_STYLE_CSS_GZ_LEN);
    }

    static esp_err_t h_redir_root(httpd_req_t* req)   { return redirect(req, "/"); }
    static esp_err_t h_redir_debug(httpd_req_t* req)  { return redirect(req, "/#debug"); }
    static esp_err_t h_redir_update(httpd_req_t* req) { return redirect(req, "/#update"); }
    static esp_err_t h_redir_pair(httpd_req_t* req)   { return redirect(req, "/#pair"); }

    static esp_err_t h_system_info(httpd_req_t* req) {
        char ip[16];
        current_ip(ip, sizeof(ip));
        std::string ssid = settings::get_wifi_ssid();
        char buf[400];
        snprintf(buf, sizeof(buf),
            "{\"fw\":\"%s\",\"uptime_s\":%lu,\"heap\":%u,"
            "\"wifi\":\"%s\",\"ip\":\"%s\",\"maintenance\":%s,"
            "\"ssid\":\"%s\",\"wifi_pw_set\":%s,\"auth\":%s}",
            FIRMWARE_VERSION,
            (unsigned long)(millis() / 1000),
            (unsigned)esp_get_free_heap_size(),
            wifi_manager::is_connected()   ? "STA" :
            wifi_manager::in_portal_mode() ? "AP"  : "none",
            ip,
            settings::get_maintenance_mode() ? "true" : "false",
            ssid.c_str(),
            settings::get_wifi_psk().empty() ? "false" : "true",
            settings::get_auth_enabled() ? "true" : "false");
        return send_ok(req, buf);
    }

    static esp_err_t h_restart(httpd_req_t* req) {
        if (!guard(req)) return ESP_OK;
        send_ok(req, "{\"ok\":true}");
        debug_log::write(debug_log::INFO, SRC, "restart via API");
        schedule_restart(300);
        return ESP_OK;
    }

    static esp_err_t h_factory_reset(httpd_req_t* req) {
        if (!guard(req)) return ESP_OK;
        send_ok(req, "{\"ok\":true}");
        debug_log::write(debug_log::WARN, SRC,
            "FACTORY RESET — wiping NVS, rebooting to AP");
        settings::factory_reset();
        schedule_restart(300);
        return ESP_OK;
    }

    static esp_err_t h_maintenance_get(httpd_req_t* req) {
        bool on = settings::get_maintenance_mode();
        char buf[32];
        snprintf(buf, sizeof(buf), "{\"maintenance\":%s}", on ? "true" : "false");
        return send_ok(req, buf);
    }

    static esp_err_t h_maintenance_post(httpd_req_t* req) {
        if (!guard(req)) return ESP_OK;
        char* body = read_body(req, 1024, nullptr);
        char val[16] = {0};
        bool on = false;
        if (body) {
            if (form_get(body, "enable", val, sizeof(val)))
                on = (strcmp(val, "1") == 0 || strcmp(val, "true") == 0);
            free(body);
        }
        settings::set_maintenance_mode(on);
        debug_log::write(debug_log::WARN, SRC,
            "maintenance mode %s — rebooting", on ? "ENABLED" : "DISABLED");
        send_ok(req, "{\"ok\":true}");
        schedule_restart(300);
        return ESP_OK;
    }

    static esp_err_t h_auth_post(httpd_req_t* req) {
        if (!guard(req)) return ESP_OK;
        char* body = read_body(req, 1024, nullptr);
        char val[16] = {0};
        bool on = false;
        if (body) {
            if (form_get(body, "enable", val, sizeof(val)))
                on = (strcmp(val, "1") == 0 || strcmp(val, "true") == 0);
            free(body);
        }
        settings::set_auth_enabled(on);
        debug_log::write(debug_log::WARN, SRC, "HTTP auth %s",
            on ? "ENABLED" : "DISABLED");
        return send_ok(req, "{\"ok\":true}");
    }

    static esp_err_t h_mqtt_get(httpd_req_t* req) {
        char buf[200];
        snprintf(buf, sizeof(buf),
            "{\"host\":\"%s\",\"port\":%u,\"user\":\"%s\",\"has_pass\":%s}",
            settings::get_mqtt_host().c_str(),
            (unsigned)settings::get_mqtt_port(),
            settings::get_mqtt_user().c_str(),
            settings::get_mqtt_pass().empty() ? "false" : "true");
        return send_ok(req, buf);
    }

    static esp_err_t h_mqtt_post(httpd_req_t* req) {
        if (!guard(req)) return ESP_OK;
        char* body = read_body(req, 1024, nullptr);
        char host[128] = {0}, port[8] = {0}, user[64] = {0}, pass[128] = {0};
        bool hh = false, hp = false, hu = false, hw = false;
        if (body) {
            hh = form_get(body, "host", host, sizeof(host));
            hp = form_get(body, "port", port, sizeof(port));
            hu = form_get(body, "user", user, sizeof(user));
            hw = form_get(body, "pass", pass, sizeof(pass));
            free(body);
        }
        settings::set_mqtt_host(hh ? host : "");
        if (hp) {
            long v = atol(port);
            if (v >= 1 && v <= 65535) settings::set_mqtt_port((uint16_t)v);
        }
        settings::set_mqtt_user(hu ? user : "");
        // Blank password = keep existing.
        if (hw && pass[0] != '\0') settings::set_mqtt_pass(pass);
        debug_log::write(debug_log::INFO, SRC, "MQTT settings saved — rebooting");
        send_ok(req, "{\"ok\":true}");
        schedule_restart(300);
        return ESP_OK;
    }

    static esp_err_t h_mowsecs_get(httpd_req_t* req) {
        char buf[32];
        snprintf(buf, sizeof(buf), "{\"secs\":%lu}",
            (unsigned long)settings::get_mow_override_secs());
        return send_ok(req, buf);
    }

    static esp_err_t h_mowsecs_post(httpd_req_t* req) {
        if (!guard(req)) return ESP_OK;
        char* body = read_body(req, 1024, nullptr);
        char val[16] = {0};
        long v = 0;
        if (body) {
            if (form_get(body, "secs", val, sizeof(val))) v = atol(val);
            free(body);
        }
        if (v < 60 || v > 86400) {
            return send_json(req, "400 Bad Request",
                "{\"error\":\"secs must be 60-86400\"}");
        }
        settings::set_mow_override_secs((uint32_t)v);
        debug_log::write(debug_log::INFO, SRC,
            "mow override duration set to %ld s", v);
        return send_ok(req, "{\"ok\":true}");
    }

    static esp_err_t h_recheck_get(httpd_req_t* req) {
        char buf[32];
        snprintf(buf, sizeof(buf), "{\"min\":%lu}",
            (unsigned long)ble_manager::get_idle_recheck_minutes());
        return send_ok(req, buf);
    }

    static esp_err_t h_recheck_post(httpd_req_t* req) {
        if (!guard(req)) return ESP_OK;
        char* body = read_body(req, 1024, nullptr);
        char val[16] = {0};
        long v = 0;
        if (body) {
            if (form_get(body, "min", val, sizeof(val))) v = atol(val);
            free(body);
        }
        if (v < 0 || v > 1440) {
            return send_json(req, "400 Bad Request",
                "{\"error\":\"min must be 0-1440 (0 = manual only)\"}");
        }
        ble_manager::set_idle_recheck_minutes((uint32_t)v);
        return send_ok(req, "{\"ok\":true}");
    }

    static esp_err_t h_wifi_scan(httpd_req_t* req) {
        // Blocking scan — acceptable for an admin action. If a scan is already
        // running esp_wifi returns an error → return an empty array.
        wifi_scan_config_t cfg = {};
        esp_err_t e = esp_wifi_scan_start(&cfg, true);
        if (e != ESP_OK) {
            return send_ok(req, "[]");
        }
        uint16_t num = 0;
        esp_wifi_scan_get_ap_num(&num);
        if (num == 0) return send_ok(req, "[]");
        if (num > 40) num = 40;
        wifi_ap_record_t* recs =
            (wifi_ap_record_t*)calloc(num, sizeof(wifi_ap_record_t));
        if (!recs) return send_ok(req, "[]");
        esp_wifi_scan_get_ap_records(&num, recs);

        DynamicJsonDocument doc(4096);
        JsonArray arr = doc.to<JsonArray>();
        for (uint16_t i = 0; i < num; i++) {
            JsonObject net = arr.createNestedObject();
            net["ssid"]   = (const char*)recs[i].ssid;
            net["rssi"]   = recs[i].rssi;
            net["secure"] = (recs[i].authmode != WIFI_AUTH_OPEN);
        }
        free(recs);
        std::string json;
        serializeJson(doc, json);
        return send_ok(req, json.c_str());
    }

    static esp_err_t h_ble_scan(httpd_req_t* req) {
        ble_manager::Result results[ble_manager::MAX_RESULTS];
        uint8_t n = ble_manager::get_results(results, ble_manager::MAX_RESULTS);
        DynamicJsonDocument doc(4096);
        doc["scanning"] = ble_manager::is_scanning();
        JsonArray arr = doc.createNestedArray("devices");
        for (uint8_t i = 0; i < n; i++) {
            JsonObject obj = arr.createNestedObject();
            obj["addr"]  = results[i].addr;
            obj["name"]  = results[i].name;
            obj["rssi"]  = results[i].rssi;
            obj["mower"] = results[i].mower;
        }
        std::string json;
        serializeJson(doc, json);
        return send_ok(req, json.c_str());
    }

    static esp_err_t h_ble_trigger(httpd_req_t* req) {
        if (!guard(req)) return ESP_OK;
        ble_manager::trigger_scan();
        return send_ok(req, "{\"ok\":true}");
    }

    static esp_err_t h_mower_status(httpd_req_t* req) {
        ble_manager::MowerStatus st = ble_manager::get_mower_status();
        const char* state_str =
            st.state == ble_manager::ConnState::IDLE          ? "idle"          :
            st.state == ble_manager::ConnState::CONNECTING    ? "connecting"    :
            st.state == ble_manager::ConnState::HANDSHAKING   ? "handshaking"   :
            st.state == ble_manager::ConnState::AUTHENTICATED ? "authenticated" :
            st.state == ble_manager::ConnState::DORMANT       ? "dormant"       :
                                                                "error";
        char ms[8], ma[8], mb[8], mns[16];
        if (st.mower_state    >= 0) snprintf(ms, sizeof(ms), "%d", (int)st.mower_state);
        else strcpy(ms, "null");
        if (st.mower_activity >= 0) snprintf(ma, sizeof(ma), "%d", (int)st.mower_activity);
        else strcpy(ma, "null");
        if (st.mower_battery  >= 0) snprintf(mb, sizeof(mb), "%d", (int)st.mower_battery);
        else strcpy(mb, "null");
        if (st.mower_next_start >= 0) snprintf(mns, sizeof(mns), "%ld", (long)st.mower_next_start);
        else strcpy(mns, "null");
        char dpw[8], cre[8];
        if (st.drive_past     >= 0) snprintf(dpw, sizeof(dpw), "%d", (int)st.drive_past);
        else strcpy(dpw, "null");
        if (st.collision_resp >= 0) snprintf(cre, sizeof(cre), "%d", (int)st.collision_resp);
        else strcpy(cre, "null");
        char buf[448];
        snprintf(buf, sizeof(buf),
            "{\"state\":\"%s\",\"addr\":\"%s\",\"name\":\"%s\","
            "\"rssi\":%d,\"detail\":\"%s\","
            "\"mower_state\":%s,\"mower_activity\":%s,\"mower_battery\":%s,"
            "\"mower_next_start\":%s,\"drive_past\":%s,\"collision_resp\":%s}",
            state_str, st.addr, st.name, (int)st.rssi, st.detail,
            ms, ma, mb, mns, dpw, cre);
        return send_ok(req, buf);
    }

    static esp_err_t h_mower_wake(httpd_req_t* req) {
        if (!guard(req)) return ESP_OK;
        ble_manager::force_wake();
        return send_ok(req, "{\"ok\":true}");
    }

    static esp_err_t h_mower_command(httpd_req_t* req) {
        if (!guard(req)) return ESP_OK;
        char* body = read_body(req, 1024, nullptr);
        char cmd[32] = {0};
        char valbuf[12] = {0};
        uint32_t val = 0;
        bool have = false;
        if (body) {
            have = form_get(body, "cmd", cmd, sizeof(cmd));
            if (form_get(body, "val", valbuf, sizeof(valbuf)))
                val = (uint32_t)strtoul(valbuf, nullptr, 10);
            free(body);
        }
        if (!have || cmd[0] == '\0') {
            return send_json(req, "400 Bad Request",
                "{\"error\":\"cmd required\"}");
        }
        if (!ble_manager::queue_command(cmd, val)) {
            return send_json(req, "400 Bad Request",
                "{\"error\":\"unknown command\"}");
        }
        debug_log::write(debug_log::INFO, SRC, "cmd queued: %s", cmd);
        return send_ok(req, "{\"ok\":true}");
    }

    static esp_err_t h_schedule_get(httpd_req_t* req) {
        ble_manager::ScheduleTask tasks[16];
        uint32_t count = 0;
        if (!ble_manager::read_schedule(tasks, 16, &count)) {
            return send_json(req, "503 Service Unavailable",
                "{\"error\":\"mower schedule unavailable\"}");
        }
        DynamicJsonDocument doc(2048);
        JsonArray arr = doc.createNestedArray("tasks");
        for (uint32_t i = 0; i < count; i++) {
            JsonObject task = arr.createNestedObject();
            emit_task_json(task, i, tasks[i]);
        }
        std::string json;
        serializeJson(doc, json);
        return send_ok(req, json.c_str());
    }

    static esp_err_t h_schedule_post(httpd_req_t* req) {
        if (!guard(req)) return ESP_OK;
        size_t blen = 0;
        char* body = read_body(req, 4096, &blen);
        if (!body || blen == 0) {
            if (body) free(body);
            return send_json(req, "400 Bad Request",
                "{\"error\":\"JSON body required\"}");
        }
        // Deserialize from a CONST char* so ArduinoJson COPIES the keys/strings
        // into the document's own pool. With the mutable (char*) overload it does
        // zero-copy and keeps pointers into `body` — and the free(body) below
        // then dangles them (ESP-IDF heap poisoning corrupts the freed block),
        // which made every containsKey()/key lookup fail. See CHANGELOG 0.17.1.
        DynamicJsonDocument doc(8192);
        auto err = deserializeJson(doc, (const char*)body);
        free(body);
        if (err) {
            return send_json(req, "400 Bad Request",
                "{\"error\":\"invalid JSON\"}");
        }
        JsonArray arr;
        if (doc.containsKey("tasks") && doc["tasks"].is<JsonArray>()) {
            arr = doc["tasks"].as<JsonArray>();
        } else if (doc.is<JsonArray>()) {
            arr = doc.as<JsonArray>();
        } else {
            return send_json(req, "400 Bad Request",
                "{\"error\":\"tasks array required\"}");
        }
        if (arr.size() > 16) {
            return send_json(req, "400 Bad Request",
                "{\"error\":\"maximum 16 tasks supported\"}");
        }
        ble_manager::ScheduleTask tasks[16];
        uint32_t count = 0;
        for (JsonVariant item : arr) {
            if (!item.is<JsonObject>()) {
                return send_json(req, "400 Bad Request",
                    "{\"error\":\"each task must be an object\"}");
            }
            JsonObject task = item.as<JsonObject>();
            uint32_t start = 0;
            uint32_t duration = 0;
            bool valid = false;
            if (task.containsKey("start")) {
                if (task["start"].is<const char*>()) {
                    valid = parse_task_time(task["start"], start);
                } else {
                    start = task["start"].as<uint32_t>();
                    valid = true;
                }
            }
            if (!valid || start >= 86400) {
                return send_json(req, "400 Bad Request",
                    "{\"error\":\"invalid task start\"}");
            }
            if (task.containsKey("duration")) {
                duration = task["duration"].as<uint32_t>();
            } else if (task.containsKey("secs")) {
                duration = task["secs"].as<uint32_t>();
            } else {
                return send_json(req, "400 Bad Request",
                    "{\"error\":\"duration required\"}");
            }
            if (duration == 0 || duration > 65535) {
                return send_json(req, "400 Bad Request",
                    "{\"error\":\"invalid task duration (max 65535 seconds)\"}");
            }
            if (task.containsKey("days") && task["days"].is<JsonArray>()) {
                if (!parse_task_days(task["days"].as<JsonArray>(),
                                     tasks[count].use_on)) {
                    return send_json(req, "400 Bad Request",
                        "{\"error\":\"invalid task days\"}");
                }
            } else {
                return send_json(req, "400 Bad Request",
                    "{\"error\":\"days array required\"}");
            }
            tasks[count].start = start;
            tasks[count].duration = duration;
            count++;
        }
        if (!ble_manager::write_schedule_wake(tasks, count)) {
            return send_json(req, "503 Service Unavailable",
                "{\"error\":\"failed to write schedule\"}");
        }
        return send_ok(req, "{\"ok\":true}");
    }

    static esp_err_t h_mower_pair(httpd_req_t* req) {
        if (!guard(req)) return ESP_OK;
        char* body = read_body(req, 1024, nullptr);
        char mac[32] = {0}, pin[16] = {0};
        bool have_mac = false, have_pin = false;
        if (body) {
            have_mac = form_get(body, "mac", mac, sizeof(mac));
            have_pin = form_get(body, "pin", pin, sizeof(pin));
            free(body);
        }
        if (!have_mac || mac[0] == '\0') {
            return send_json(req, "400 Bad Request",
                "{\"error\":\"mac required\"}");
        }
        uint32_t pinv = have_pin ? (uint32_t)atol(pin) : 1234;
        ble_manager::set_target(mac, pinv);
        debug_log::write(debug_log::INFO, SRC,
            "pair request mac=%s pin=%u", mac, (unsigned)pinv);
        return send_ok(req, "{\"ok\":true}");
    }

    static esp_err_t h_mower_disconnect(httpd_req_t* req) {
        if (!guard(req)) return ESP_OK;
        ble_manager::disconnect_target();
        return send_ok(req, "{\"ok\":true}");
    }

    static esp_err_t h_wifi_post(httpd_req_t* req) {
        if (!guard(req)) return ESP_OK;
        char* body = read_body(req, 1024, nullptr);
        char ssid[64] = {0}, psk[64] = {0};
        bool have_psk = false;
        if (body) {
            form_get(body, "ssid", ssid, sizeof(ssid));
            have_psk = form_get(body, "psk", psk, sizeof(psk));
            free(body);
        }
        if (ssid[0] == '\0') {
            return send_json(req, "400 Bad Request",
                "{\"error\":\"ssid required\"}");
        }
        settings::set_wifi_ssid(ssid);
        // Blank password = keep the stored one.
        if (have_psk && psk[0] != '\0') settings::set_wifi_psk(psk);
        debug_log::write(debug_log::INFO, SRC,
            "WiFi creds saved ssid=%s — restarting", ssid);
        send_ok(req, "{\"ok\":true}");
        schedule_restart(300);
        return ESP_OK;
    }

    static esp_err_t h_admin_post(httpd_req_t* req) {
        if (!guard(req)) return ESP_OK;
        char* body = read_body(req, 1024, nullptr);
        char user[64] = {0}, new_pass[64] = {0};
        bool have_pass = false;
        if (body) {
            form_get(body, "user", user, sizeof(user));
            have_pass = form_get(body, "new_pass", new_pass, sizeof(new_pass));
            free(body);
        }
        if (!have_pass || new_pass[0] == '\0') {
            return send_json(req, "400 Bad Request",
                "{\"error\":\"new_pass required\"}");
        }
        if (user[0] != '\0') settings::set_admin_user(user);
        settings::set_admin_pass(new_pass);
        debug_log::write(debug_log::INFO, SRC, "admin credentials updated");
        return send_ok(req, "{\"ok\":true}");
    }

    // ── OTA: raw firmware body → esp_ota ─────────────────────────────────
    static esp_err_t h_ota_post(httpd_req_t* req) {
        if (!guard(req)) return ESP_OK;

        // Cease ALL BLE activity before flashing: drops any mower link, stops
        // scanning, and idles the conn_task so the OTA owns the CPU, radio and
        // heap (and a reconnect can't churn mid-flash). One-way — we reboot
        // after a successful flash. Brief settle lets the link teardown finish.
        ble_manager::suspend();
        vTaskDelay(pdMS_TO_TICKS(200));

        const esp_partition_t* part = esp_ota_get_next_update_partition(NULL);
        if (!part) {
            debug_log::write(debug_log::ERROR, SRC, "no OTA partition");
            return send_ok(req, "{\"error\":\"flash failed\"}");
        }
        esp_ota_handle_t handle = 0;
        if (esp_ota_begin(part, OTA_SIZE_UNKNOWN, &handle) != ESP_OK) {
            debug_log::write(debug_log::ERROR, SRC, "esp_ota_begin failed");
            return send_ok(req, "{\"error\":\"flash failed\"}");
        }
        debug_log::write(debug_log::INFO, SRC, "OTA upload start");

        // The web UI posts multipart/form-data (FormData field "firmware"), so
        // the raw body is wrapped in a MIME envelope (a part header ended by
        // CRLFCRLF, the firmware bytes, then a closing "\r\n--<boundary>--").
        // Both must be stripped or the flashed image is corrupted. Detect the
        // boundary from Content-Type; a non-multipart (raw) body streams as-is.
        char marker[96]; int mlen = 0;
        {
            char ct[200];
            if (httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct)) == ESP_OK
                && strstr(ct, "multipart/form-data")) {
                char* b = strstr(ct, "boundary=");
                if (b) {
                    b += 9;
                    if (*b == '"') b++;
                    char bnd[80]; size_t bi = 0;
                    while (*b && *b != '"' && *b != ';' && bi < sizeof(bnd) - 1)
                        bnd[bi++] = *b++;
                    bnd[bi] = '\0';
                    if (bi > 0) mlen = snprintf(marker, sizeof(marker), "\r\n--%s", bnd);
                }
            }
        }
        bool multipart = (mlen > 0);
        debug_log::write(debug_log::INFO, SRC, "OTA body: %s",
                         multipart ? "multipart/form-data" : "raw");

        char buf[1024];
        char pre[1024]; size_t pre_len = 0;   // multipart preamble accumulator
        char carry[96]; int carry_len = 0;    // possible split closing marker
        bool ok = true, done = false, in_body = !multipart;
        size_t total = 0;
        int remaining = (int)req->content_len;

        while (remaining > 0 && !done && ok) {
            int n = httpd_req_recv(req, buf,
                                   remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf));
            if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
            if (n <= 0) { ok = false; break; }
            remaining -= n;

            const char* data = buf; int len = n;

            // Phase 1: skip the multipart preamble (up to the first CRLFCRLF).
            if (!in_body) {
                size_t take = sizeof(pre) - pre_len;
                if (take > (size_t)len) take = (size_t)len;
                memcpy(pre + pre_len, data, take);
                pre_len += take;
                int hdr_end = -1;
                for (size_t i = 0; i + 4 <= pre_len; i++)
                    if (memcmp(pre + i, "\r\n\r\n", 4) == 0) { hdr_end = (int)i + 4; break; }
                if (hdr_end < 0) {
                    if (pre_len >= sizeof(pre)) ok = false; // part header too large
                    continue;
                }
                in_body = true;
                data = pre + hdr_end;
                len  = (int)pre_len - hdr_end;
            }

            // Phase 2: write body, holding back a possible split closing marker.
            int slen = carry_len + len;
            char* s = (char*)malloc(slen > 0 ? slen : 1);
            if (!s) { ok = false; break; }
            if (carry_len) memcpy(s, carry, carry_len);
            if (len > 0)   memcpy(s + carry_len, data, len);

            int found = -1;
            if (multipart)
                for (int i = 0; i + mlen <= slen; i++)
                    if (memcmp(s + i, marker, mlen) == 0) { found = i; break; }

            if (found >= 0) {
                if (found > 0) {
                    if (esp_ota_write(handle, s, found) != ESP_OK) ok = false;
                    else total += (size_t)found;
                }
                done = true;            // hit the closing boundary
            } else {
                int keep = multipart ? (mlen - 1) : 0;
                if (keep > slen) keep = slen;
                int wr = slen - keep;
                if (wr > 0) {
                    if (esp_ota_write(handle, s, wr) != ESP_OK) ok = false;
                    else total += (size_t)wr;
                }
                if (keep > 0) memmove(carry, s + wr, keep);
                carry_len = keep;
            }
            free(s);
        }
        // No closing marker (raw upload, or malformed): flush any retained tail.
        if (ok && !done && carry_len > 0) {
            if (esp_ota_write(handle, carry, carry_len) == ESP_OK) total += (size_t)carry_len;
            else ok = false;
        }
        // Drain any unread remainder so the socket closes cleanly.
        while (remaining > 0) {
            int n = httpd_req_recv(req, buf,
                                   remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf));
            if (n <= 0) break;
            remaining -= n;
        }

        // A stream/write error happened before esp_ota_end: the handle is still
        // open, so abort it to release it (otherwise it leaks for this boot and
        // a retry fails). esp_ota_end frees the handle itself, so it must NOT be
        // followed by esp_ota_abort.
        if (!ok) {
            esp_ota_abort(handle);
            debug_log::write(debug_log::ERROR, SRC, "OTA flash error (upload)");
            return send_ok(req, "{\"error\":\"flash failed\"}");
        }

        esp_err_t end_rc = esp_ota_end(handle); // frees handle (success or fail)
        if (end_rc != ESP_OK) {
            debug_log::write(debug_log::ERROR, SRC, "esp_ota_end failed (rc=%d)", end_rc);
            return send_ok(req, "{\"error\":\"flash failed\"}");
        }
        if (esp_ota_set_boot_partition(part) != ESP_OK) {
            debug_log::write(debug_log::ERROR, SRC, "set_boot_partition failed");
            return send_ok(req, "{\"error\":\"flash failed\"}");
        }

        debug_log::write(debug_log::INFO, SRC,
            "OTA flash OK (%u bytes) — restarting", (unsigned)total);
        // NimBLE panics on the first boot after an OTA soft-reset; force one
        // extra clean reboot before BLE init.
        settings::set_post_ota(1);
        send_ok(req, "{\"ok\":true}");
        schedule_restart(1000);
        return ESP_OK;
    }

    // ── SSE: debug-log stream ────────────────────────────────────────────
    // esp_http_server runs ALL requests on ONE task, so a handler that blocks
    // (the SSE live loop) would hang every other request — page loads, the
    // maintenance button, status polls, the lot. So the stream runs on its OWN
    // task via httpd_req_async_handler_begin(); h_sse returns immediately and
    // the httpd task stays free.
    static void sse_worker(void* arg) {
        httpd_req_t* req = (httpd_req_t*)arg;
        int fd = httpd_req_to_sockfd(req);

        httpd_resp_set_type(req, "text/event-stream");
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
        httpd_resp_set_hdr(req, "Connection", "keep-alive");

        // Replay the bounded tail of the ring buffer.
        struct ReplayCtx { httpd_req_t* req; bool ok; };
        ReplayCtx rc{req, true};
        debug_log::visit_all([](const char* json, uint32_t id, void* p) {
            ReplayCtx* cx = (ReplayCtx*)p;
            if (!cx->ok) return;
            if (sse_send_frame(cx->req, json, id) != ESP_OK) cx->ok = false;
        }, &rc, SSE_REPLAY_MAX);

        // Live loop; keepalive on idle so a dead socket is detected.
        const TickType_t to = pdMS_TO_TICKS(10000);
        while (rc.ok) {
            SseMsg m;
            if (_sse_q && xQueueReceive(_sse_q, &m, to) == pdTRUE) {
                if (sse_send_frame(req, m.json, m.id) != ESP_OK) break;
            } else {
                if (httpd_resp_send_chunk(req, ": keepalive\n\n", 13) != ESP_OK) break;
            }
        }

        if (_sse_fd == fd) _sse_fd = -1;
        httpd_resp_send_chunk(req, NULL, 0);     // end the chunked response
        httpd_req_async_handler_complete(req);   // release the detached request
        vTaskDelete(nullptr);
    }

    static esp_err_t h_sse(httpd_req_t* req) {
        // One SSE client only (one admin UI). Reject a second so it can't tie up
        // another socket. Claim the slot now to close the race with the worker.
        if (_sse_fd >= 0) {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_sendstr(req, "SSE already connected");
            return ESP_OK;
        }
        _sse_fd = httpd_req_to_sockfd(req);

        // Detach the request to a worker task so the httpd task is freed at once.
        httpd_req_t* areq = nullptr;
        if (httpd_req_async_handler_begin(req, &areq) != ESP_OK) {
            _sse_fd = -1;
            httpd_resp_set_status(req, "503 Service Unavailable");
            httpd_resp_sendstr(req, "SSE unavailable");
            return ESP_OK;
        }
        if (xTaskCreate(sse_worker, "sse", 4096, areq, 5, nullptr) != pdPASS) {
            httpd_req_async_handler_complete(areq);
            _sse_fd = -1;
            return ESP_OK;
        }
        return ESP_OK; // httpd task is immediately free for other requests
    }

    // ── captive-portal 404 / fallback ────────────────────────────────────
    static esp_err_t h_404(httpd_req_t* req, httpd_err_code_t /*err*/) {
        if (wifi_manager::in_portal_mode()) {
            return redirect(req, "/");
        }
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Not found");
        return ESP_OK;
    }

    // Browsers auto-request /favicon.ico; we ship no icon. Answer 204 No Content
    // so httpd stops logging "URI '/favicon.ico' not found" on every page load.
    static esp_err_t h_favicon(httpd_req_t* req) {
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    // Register a GET route.
    static void reg(const char* uri, httpd_method_t method,
                    esp_err_t (*fn)(httpd_req_t*)) {
        httpd_uri_t u = {};
        u.uri = uri;
        u.method = method;
        u.handler = fn;
        httpd_register_uri_handler(_server, &u);
    }

    void begin() {
        // Wire SSE broadcast so every debug_log::write() is pushed to clients.
        _sse_q = xQueueCreate(SSE_Q_DEPTH, sizeof(SseMsg));
        debug_log::set_broadcast(sse_broadcast);

        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.server_port      = 80;
        config.max_uri_handlers = 48;
        config.stack_size       = 8192;
        // Raise the httpd task above the BLE conn_task (prio 1) and the default
        // (5) so web requests are serviced even while BLE is busy. Kept below
        // the lwip TCP/IP task (18) so inbound packets are still delivered first.
        config.task_priority    = 10;
        config.lru_purge_enable = true;
        // Max is 7 with the default LWIP_MAX_SOCKETS (10) − 3 used internally by
        // httpd; going higher makes httpd_start() fail. The SSE single-client
        // guard (see h_sse) is what prevents a blocked SSE socket from starving
        // normal pages, so 7 is sufficient.
        config.max_open_sockets  = 7;
        config.uri_match_fn      = httpd_uri_match_wildcard;

        if (httpd_start(&_server, &config) != ESP_OK) {
            debug_log::write(debug_log::ERROR, SRC, "httpd_start failed");
            return;
        }

        // Browser noise: no favicon → 204 (silences repeated httpd 404 warnings)
        reg("/favicon.ico",               HTTP_GET, h_favicon);

        // Captive-portal detectors → root
        reg("/generate_204",              HTTP_GET, h_redir_root);
        reg("/hotspot-detect.html",       HTTP_GET, h_redir_root);
        reg("/library/test/success.html", HTTP_GET, h_redir_root);
        reg("/connecttest.txt",           HTTP_GET, h_redir_root);
        reg("/ncsi.txt",                  HTTP_GET, h_redir_root);

        // Pages
        reg("/",          HTTP_GET, h_root);
        reg("/setup",     HTTP_GET, h_redir_root);
        reg("/debug",     HTTP_GET, h_redir_debug);
        reg("/update",    HTTP_GET, h_redir_update);
        reg("/pair",      HTTP_GET, h_redir_pair);
        reg("/style.css", HTTP_GET, h_style);

        // API
        reg("/api/system/info",          HTTP_GET,  h_system_info);
        reg("/api/system/restart",       HTTP_POST, h_restart);
        reg("/api/system/factory-reset", HTTP_POST, h_factory_reset);
        reg("/api/system/maintenance",   HTTP_GET,  h_maintenance_get);
        reg("/api/system/maintenance",   HTTP_POST, h_maintenance_post);
        reg("/api/auth",                 HTTP_POST, h_auth_post);
        reg("/api/mqtt",                 HTTP_GET,  h_mqtt_get);
        reg("/api/mqtt",                 HTTP_POST, h_mqtt_post);
        reg("/api/mower/mow-secs",       HTTP_GET,  h_mowsecs_get);
        reg("/api/mower/mow-secs",       HTTP_POST, h_mowsecs_post);
        reg("/api/ble/recheck",          HTTP_GET,  h_recheck_get);
        reg("/api/ble/recheck",          HTTP_POST, h_recheck_post);
        reg("/api/wifi/scan",            HTTP_GET,  h_wifi_scan);
        reg("/api/ble/scan",             HTTP_GET,  h_ble_scan);
        reg("/api/ble/trigger",          HTTP_POST, h_ble_trigger);
        reg("/api/mower/status",         HTTP_GET,  h_mower_status);
        reg("/api/mower/wake",           HTTP_POST, h_mower_wake);
        reg("/api/mower/command",        HTTP_POST, h_mower_command);
        reg("/api/mower/schedule",       HTTP_GET,  h_schedule_get);
        reg("/api/mower/schedule",       HTTP_POST, h_schedule_post);
        reg("/api/mower/pair",           HTTP_POST, h_mower_pair);
        reg("/api/mower/disconnect",     HTTP_POST, h_mower_disconnect);
        reg("/api/wifi",                 HTTP_POST, h_wifi_post);
        reg("/api/admin",                HTTP_POST, h_admin_post);
        reg("/api/ota",                  HTTP_POST, h_ota_post);
        reg("/api/debug/stream",         HTTP_GET,  h_sse);

        // 404 / captive-portal fallback
        httpd_register_err_handler(_server, HTTPD_404_NOT_FOUND, h_404);

        debug_log::write(debug_log::INFO, SRC, "HTTP server started on port 80");
    }

    // No-op: the SSE handler drains its own queue; ArduinoOTA is gone.
    void loop() {}
}
