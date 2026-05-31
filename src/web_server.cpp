// SPDX-License-Identifier: GPL-3.0-or-later
#include "web_server.h"
#include "config.h"
#include "settings.h"
#include "debug_log.h"
#include "wifi_manager.h"
#include "ble/ble_manager.h"
#include <ESPAsyncWebServer.h>
#include "web_assets.h"   // gzipped UI embedded in flash (generated)
#include <WiFi.h>
#include <Update.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <freertos/timers.h>
#include <freertos/queue.h>
#include <mbedtls/base64.h>

#define SRC "web"

// Auth is intentionally disabled while on the bench (M2/M3).
// Restore check_auth calls once Basic Auth UX is sorted.

namespace web_server {

    static AsyncWebServer  server(80);
    static AsyncEventSource _sse("/api/debug/stream");

    // ── SSE marshalling ──────────────────────────────────────────────────
    // AsyncTCP is NOT thread-safe: _sse.send() must only ever be touched from
    // ONE task. debug_log::write() is called from many (BLE conn task, WiFi
    // callbacks, Arduino loop), so producers only enqueue here; the single
    // drain in loop() does the actual _sse.send(). Bounded depth — on a burst
    // we drop live frames (the ring buffer + connect replay is the source of
    // truth, so a dropped live line is cosmetic, not lost).
    struct SseMsg {
        uint32_t id;
        char     json[DEBUG_LOG_LINE_MAX * 2 + 80];
    };
    static constexpr uint8_t  SSE_Q_DEPTH    = 12; // ~5.6 KB heap (was 24)
    static constexpr uint8_t  SSE_DRAIN_MAX  = 12; // max sends per loop tick
    static constexpr uint16_t SSE_REPLAY_MAX = 40; // connect-time backlog cap;
                                                   // halves the visit_all()
                                                   // malloc spike to ~8.5 KB
    static QueueHandle_t _sse_q = nullptr;

    static void schedule_restart(uint32_t ms) {
        TimerHandle_t t = xTimerCreate("rst", pdMS_TO_TICKS(ms),
            pdFALSE, nullptr, [](TimerHandle_t) { ESP.restart(); });
        if (t) xTimerStart(t, 0);
        else   ESP.restart();
    }

    // HTTP Basic Auth gate. Returns true if the request may proceed. When
    // auth is enabled, validates the Authorization header against the salted
    // SHA-256 admin credentials (settings::verify_admin) and otherwise sends
    // a 401 challenge. Default-off: if disabled this is a no-op. Recovery if
    // locked out = BOOT-hold NVS wipe (physical, auth-independent).
    static bool guard(AsyncWebServerRequest* req) {
        if (!settings::get_auth_enabled()) return true;
        if (req->hasHeader("Authorization")) {
            String h = req->getHeader("Authorization")->value();
            if (h.startsWith("Basic ")) {
                String b64 = h.substring(6);
                unsigned char out[160]; size_t olen = 0;
                if (mbedtls_base64_decode(out, sizeof(out) - 1, &olen,
                        (const unsigned char*)b64.c_str(), b64.length()) == 0) {
                    out[olen] = 0;
                    String creds = (char*)out;
                    int c = creds.indexOf(':');
                    if (c > 0 &&
                        settings::verify_admin(creds.substring(0, c),
                                               creds.substring(c + 1)))
                        return true;
                }
            }
        }
        req->requestAuthentication();
        return false;
    }

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
        String s(text);
        s.toLowerCase();
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

    // Serve a gzip-compressed PROGMEM blob. The asset ships inside the
    // firmware binary, so there is no "filesystem not flashed" failure mode.
    static void send_gz(AsyncWebServerRequest* req, const char* ctype,
                        const uint8_t* body, size_t len) {
        AsyncWebServerResponse* res =
            req->beginResponse_P(200, ctype, body, len);
        res->addHeader("Content-Encoding", "gzip");
        req->send(res);
    }

    static void serve_index(AsyncWebServerRequest* req) {
        if (!guard(req)) return;
        send_gz(req, "text/html", WEB_INDEX_HTML_GZ, WEB_INDEX_HTML_GZ_LEN);
    }

    // SSE broadcast: called by debug_log on every new entry, from ANY task.
    // Only enqueues — never touches AsyncTCP here. Drops if the queue is full.
    static void sse_broadcast(const char* json, uint32_t id) {
        if (!_sse_q || _sse.count() == 0) return;
        SseMsg m;
        m.id = id;
        strncpy(m.json, json, sizeof(m.json) - 1);
        m.json[sizeof(m.json) - 1] = '\0';
        xQueueSend(_sse_q, &m, 0); // non-blocking; drop on full
    }

    // Drains queued SSE messages — runs ONLY on the Arduino loop task (the
    // single owner of _sse). Bounded per tick so a flood can't stall loop().
    static void sse_drain() {
        if (!_sse_q) return;
        SseMsg m;
        for (uint8_t n = 0; n < SSE_DRAIN_MAX &&
             xQueueReceive(_sse_q, &m, 0) == pdTRUE; ++n) {
            if (_sse.count() != 0) _sse.send(m.json, "log", m.id);
        }
    }

    void begin() {
        // Web UI is embedded in flash (web_assets.h) — no LittleFS to mount.

        // Wire SSE broadcast so every debug_log::write() is pushed to clients.
        // Queue must exist before the broadcast hook can fire.
        _sse_q = xQueueCreate(SSE_Q_DEPTH, sizeof(SseMsg));
        debug_log::set_broadcast(sse_broadcast);

        // ---- ArduinoOTA (PlatformIO wireless upload) ----
        ArduinoOTA.setHostname(MDNS_HOSTNAME);
        ArduinoOTA.onStart([]() {
            debug_log::write(debug_log::INFO, SRC, "ArduinoOTA start");
        });
        ArduinoOTA.onEnd([]() {
            debug_log::write(debug_log::INFO, SRC, "ArduinoOTA done — restarting");
        });
        ArduinoOTA.onError([](ota_error_t e) {
            debug_log::write(debug_log::ERROR, SRC, "ArduinoOTA error %u", (unsigned)e);
        });
        ArduinoOTA.begin();
        debug_log::write(debug_log::INFO, SRC, "ArduinoOTA ready");

        // ---- Captive-portal detectors → root ----
        auto to_root = [](AsyncWebServerRequest* r){ r->redirect("/"); };
        server.on("/generate_204",              HTTP_GET, to_root);
        server.on("/hotspot-detect.html",       HTTP_GET, to_root);
        server.on("/library/test/success.html", HTTP_GET, to_root);
        server.on("/connecttest.txt",           HTTP_GET, to_root);
        server.on("/ncsi.txt",                  HTTP_GET, to_root);

        // ---- Pages ----
        server.on("/",       HTTP_GET, serve_index);
        server.on("/setup",  HTTP_GET, [](AsyncWebServerRequest* r){ r->redirect("/"); });
        server.on("/debug",  HTTP_GET, [](AsyncWebServerRequest* r){ r->redirect("/#debug"); });
        server.on("/update", HTTP_GET, [](AsyncWebServerRequest* r){ r->redirect("/#update"); });
        server.on("/pair",    HTTP_GET, [](AsyncWebServerRequest* r){ r->redirect("/#pair"); });

        server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest* r){
            send_gz(r, "text/css", WEB_STYLE_CSS_GZ, WEB_STYLE_CSS_GZ_LEN);
        });

        // ---- API: system info ----
        server.on("/api/system/info", HTTP_GET, [](AsyncWebServerRequest* r){
            String ip = wifi_manager::is_connected()
                        ? WiFi.localIP().toString()
                        : WiFi.softAPIP().toString();
            String ssid = settings::get_wifi_ssid();
            char buf[400];
            snprintf(buf, sizeof(buf),
                "{\"fw\":\"%s\",\"uptime_s\":%lu,\"heap\":%u,"
                "\"wifi\":\"%s\",\"ip\":\"%s\",\"maintenance\":%s,"
                "\"ssid\":\"%s\",\"wifi_pw_set\":%s,\"auth\":%s}",
                FIRMWARE_VERSION,
                (unsigned long)(millis() / 1000),
                (unsigned)ESP.getFreeHeap(),
                wifi_manager::is_connected()   ? "STA" :
                wifi_manager::in_portal_mode() ? "AP"  : "none",
                ip.c_str(),
                settings::get_maintenance_mode() ? "true" : "false",
                ssid.c_str(),
                settings::get_wifi_psk().isEmpty() ? "false" : "true",
                settings::get_auth_enabled() ? "true" : "false");
            r->send(200, "application/json", buf);
        });

        // ---- API: restart ----
        server.on("/api/system/restart", HTTP_POST, [](AsyncWebServerRequest* r){ if (!guard(r)) return;
            r->send(200, "application/json", "{\"ok\":true}");
            debug_log::write(debug_log::INFO, SRC, "restart via API");
            schedule_restart(300);
        });

        // ---- API: factory reset ----
        server.on("/api/system/factory-reset", HTTP_POST, [](AsyncWebServerRequest* r){ if (!guard(r)) return;
            r->send(200, "application/json", "{\"ok\":true}");
            debug_log::write(debug_log::WARN, SRC, "FACTORY RESET — wiping NVS, rebooting to AP");
            settings::factory_reset();
            schedule_restart(300);
        });

        // ---- API: maintenance mode ----
        server.on("/api/system/maintenance", HTTP_GET, [](AsyncWebServerRequest* r){
            bool on = settings::get_maintenance_mode();
            char buf[32];
            snprintf(buf, sizeof(buf), "{\"maintenance\":%s}", on ? "true" : "false");
            r->send(200, "application/json", buf);
        });
        server.on("/api/system/maintenance", HTTP_POST, [](AsyncWebServerRequest* r){ if (!guard(r)) return;
            const AsyncWebParameter* p = r->getParam("enable", true);
            bool on = p && (p->value() == "1" || p->value() == "true");
            settings::set_maintenance_mode(on);
            debug_log::write(debug_log::WARN, SRC,
                "maintenance mode %s — rebooting", on ? "ENABLED" : "DISABLED");
            r->send(200, "application/json", "{\"ok\":true}");
            schedule_restart(300);
        });

        // ---- API: enable/disable HTTP Basic Auth ----
        // Default off. Enabling protects the SPA + all mutating endpoints.
        // If you get locked out, BOOT-hold (>5 s) wipes NVS incl. this flag.
        server.on("/api/auth", HTTP_POST, [](AsyncWebServerRequest* r){ if (!guard(r)) return;
            const AsyncWebParameter* p = r->getParam("enable", true);
            bool on = p && (p->value() == "1" || p->value() == "true");
            settings::set_auth_enabled(on);
            debug_log::write(debug_log::WARN, SRC,
                "HTTP auth %s", on ? "ENABLED" : "DISABLED");
            r->send(200, "application/json", "{\"ok\":true}");
        });

        // ---- API: MQTT broker settings ----
        server.on("/api/mqtt", HTTP_GET, [](AsyncWebServerRequest* r){
            char buf[200];
            snprintf(buf, sizeof(buf),
                "{\"host\":\"%s\",\"port\":%u,\"user\":\"%s\",\"has_pass\":%s}",
                settings::get_mqtt_host().c_str(),
                (unsigned)settings::get_mqtt_port(),
                settings::get_mqtt_user().c_str(),
                settings::get_mqtt_pass().isEmpty() ? "false" : "true");
            r->send(200, "application/json", buf);
        });
        server.on("/api/mqtt", HTTP_POST, [](AsyncWebServerRequest* r){ if (!guard(r)) return;
            const AsyncWebParameter* ph = r->getParam("host", true);
            const AsyncWebParameter* pp = r->getParam("port", true);
            const AsyncWebParameter* pu = r->getParam("user", true);
            const AsyncWebParameter* pw = r->getParam("pass", true);
            settings::set_mqtt_host(ph ? ph->value() : "");
            if (pp) {
                long v = pp->value().toInt();
                if (v >= 1 && v <= 65535) settings::set_mqtt_port((uint16_t)v);
            }
            settings::set_mqtt_user(pu ? pu->value() : "");
            // Blank password = keep existing (so it isn't wiped on every save).
            if (pw && !pw->value().isEmpty()) settings::set_mqtt_pass(pw->value());
            debug_log::write(debug_log::INFO, SRC,
                "MQTT settings saved — rebooting");
            r->send(200, "application/json", "{\"ok\":true}");
            schedule_restart(300);
        });

        // ---- API: mow override duration ----
        server.on("/api/mower/mow-secs", HTTP_GET, [](AsyncWebServerRequest* r){
            char buf[32];
            snprintf(buf, sizeof(buf), "{\"secs\":%lu}",
                (unsigned long)settings::get_mow_override_secs());
            r->send(200, "application/json", buf);
        });
        server.on("/api/mower/mow-secs", HTTP_POST, [](AsyncWebServerRequest* r){ if (!guard(r)) return;
            const AsyncWebParameter* p = r->getParam("secs", true);
            long v = p ? p->value().toInt() : 0;
            if (v < 60 || v > 86400) {
                r->send(400, "application/json",
                    "{\"error\":\"secs must be 60-86400\"}");
                return;
            }
            settings::set_mow_override_secs((uint32_t)v);
            debug_log::write(debug_log::INFO, SRC,
                "mow override duration set to %ld s", v);
            r->send(200, "application/json", "{\"ok\":true}");
        });

        // ---- API: WiFi scan ----
        server.on("/api/wifi/scan", HTTP_GET, [](AsyncWebServerRequest* r){
            int n = WiFi.scanComplete();
            if (n == WIFI_SCAN_RUNNING) {
                r->send(200, "application/json", "{\"scanning\":true}");
                return;
            }
            if (n < 0) {
                WiFi.scanNetworks(/*async=*/true);
                r->send(200, "application/json", "{\"scanning\":true}");
                return;
            }
            JsonDocument doc;
            JsonArray arr = doc.to<JsonArray>();
            for (int i = 0; i < n; i++) {
                JsonObject net = arr.add<JsonObject>();
                net["ssid"]   = WiFi.SSID(i);
                net["rssi"]   = WiFi.RSSI(i);
                net["secure"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
            }
            WiFi.scanDelete();
            WiFi.scanNetworks(/*async=*/true);
            String json;
            serializeJson(doc, json);
            r->send(200, "application/json", json);
        });

        // ---- API: BLE scan results ----
        server.on("/api/ble/scan", HTTP_GET, [](AsyncWebServerRequest* r){
            ble_manager::Result results[ble_manager::MAX_RESULTS];
            uint8_t n = ble_manager::get_results(results, ble_manager::MAX_RESULTS);
            JsonDocument doc;
            doc["scanning"] = ble_manager::is_scanning();
            JsonArray arr = doc["devices"].to<JsonArray>();
            for (uint8_t i = 0; i < n; i++) {
                JsonObject obj = arr.add<JsonObject>();
                obj["addr"]  = results[i].addr;
                obj["name"]  = results[i].name;
                obj["rssi"]  = results[i].rssi;
                obj["mower"] = results[i].mower;
            }
            String json;
            serializeJson(doc, json);
            r->send(200, "application/json", json);
        });

        // ---- API: trigger BLE scan ----
        server.on("/api/ble/trigger", HTTP_POST, [](AsyncWebServerRequest* r){ if (!guard(r)) return;
            ble_manager::trigger_scan();
            r->send(200, "application/json", "{\"ok\":true}");
        });

        // ---- API: mower connection status ----
        server.on("/api/mower/status", HTTP_GET, [](AsyncWebServerRequest* r){
            ble_manager::MowerStatus st = ble_manager::get_mower_status();
            const char* state_str =
                st.state == ble_manager::ConnState::IDLE         ? "idle"         :
                st.state == ble_manager::ConnState::CONNECTING   ? "connecting"   :
                st.state == ble_manager::ConnState::HANDSHAKING  ? "handshaking"  :
                st.state == ble_manager::ConnState::AUTHENTICATED? "authenticated":
                st.state == ble_manager::ConnState::DORMANT      ? "dormant"      :
                                                                   "error";
            // Telemetry values: -1 means not yet polled → JSON null.
            char ms[8], ma[8], mb[8];
            if (st.mower_state    >= 0) snprintf(ms, sizeof(ms), "%d", (int)st.mower_state);
            else strcpy(ms, "null");
            if (st.mower_activity >= 0) snprintf(ma, sizeof(ma), "%d", (int)st.mower_activity);
            else strcpy(ma, "null");
            if (st.mower_battery  >= 0) snprintf(mb, sizeof(mb), "%d", (int)st.mower_battery);
            else strcpy(mb, "null");
            char buf[384];
            snprintf(buf, sizeof(buf),
                "{\"state\":\"%s\",\"addr\":\"%s\",\"name\":\"%s\","
                "\"rssi\":%d,\"detail\":\"%s\","
                "\"mower_state\":%s,\"mower_activity\":%s,\"mower_battery\":%s}",
                state_str, st.addr, st.name, (int)st.rssi, st.detail,
                ms, ma, mb);
            r->send(200, "application/json", buf);
        });

        // ---- API: wake mower (reset backoff, trigger immediate reconnect) ----
        server.on("/api/mower/wake", HTTP_POST, [](AsyncWebServerRequest* r){ if (!guard(r)) return;
            ble_manager::force_wake();
            r->send(200, "application/json", "{\"ok\":true}");
        });

        // ---- API: queue mower command (mow | park | pause) ----
        server.on("/api/mower/command", HTTP_POST, [](AsyncWebServerRequest* r){ if (!guard(r)) return;
            const AsyncWebParameter* pc = r->getParam("cmd", true);
            if (!pc || pc->value().isEmpty()) {
                r->send(400, "application/json", "{\"error\":\"cmd required\"}");
                return;
            }
            if (!ble_manager::queue_command(pc->value().c_str(), 0)) {
                r->send(400, "application/json", "{\"error\":\"unknown command\"}");
                return;
            }
            debug_log::write(debug_log::INFO, SRC, "cmd queued: %s", pc->value().c_str());
            r->send(200, "application/json", "{\"ok\":true}");
        });

        server.on("/api/mower/schedule", HTTP_GET, [](AsyncWebServerRequest* r){
            ble_manager::ScheduleTask tasks[16];
            uint32_t count = 0;
            if (!ble_manager::read_schedule(tasks, 16, &count)) {
                r->send(503, "application/json",
                    "{\"error\":\"mower schedule unavailable\"}");
                return;
            }
            DynamicJsonDocument doc(2048);
            JsonArray arr = doc.createNestedArray("tasks");
            for (uint32_t i = 0; i < count; i++) {
                JsonObject task = arr.createNestedObject();
                emit_task_json(task, i, tasks[i]);
            }
            String json;
            serializeJson(doc, json);
            r->send(200, "application/json", json);
        });

        server.on("/api/mower/schedule", HTTP_POST, [](AsyncWebServerRequest* r){ if (!guard(r)) return;
            String body = r->hasArg("plain") ? r->arg("plain") : String();
            if (body.isEmpty()) {
                r->send(400, "application/json",
                    "{\"error\":\"JSON body required\"}");
                return;
            }
            DynamicJsonDocument doc(4096);
            auto err = deserializeJson(doc, body);
            if (err) {
                r->send(400, "application/json",
                    "{\"error\":\"invalid JSON\"}");
                return;
            }
            JsonArray arr;
            if (doc.containsKey("tasks") && doc["tasks"].is<JsonArray>()) {
                arr = doc["tasks"].as<JsonArray>();
            } else if (doc.is<JsonArray>()) {
                arr = doc.as<JsonArray>();
            } else {
                r->send(400, "application/json",
                    "{\"error\":\"tasks array required\"}");
                return;
            }
            if (arr.size() > 16) {
                r->send(400, "application/json",
                    "{\"error\":\"maximum 16 tasks supported\"}");
                return;
            }
            ble_manager::ScheduleTask tasks[16];
            uint32_t count = 0;
            for (JsonVariant item : arr) {
                if (!item.is<JsonObject>()) {
                    r->send(400, "application/json",
                        "{\"error\":\"each task must be an object\"}");
                    return;
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
                    r->send(400, "application/json",
                        "{\"error\":\"invalid task start\"}");
                    return;
                }
                if (task.containsKey("duration")) {
                    duration = task["duration"].as<uint32_t>();
                } else if (task.containsKey("secs")) {
                    duration = task["secs"].as<uint32_t>();
                } else {
                    r->send(400, "application/json",
                        "{\"error\":\"duration required\"}");
                    return;
                }
                if (duration == 0 || duration > 65535) {
                    r->send(400, "application/json",
                        "{\"error\":\"invalid task duration (max 65535 seconds)\"}");
                    return;
                }
                if (task.containsKey("days") && task["days"].is<JsonArray>()) {
                    if (!parse_task_days(task["days"].as<JsonArray>(), tasks[count].use_on)) {
                        r->send(400, "application/json",
                            "{\"error\":\"invalid task days\"}");
                        return;
                    }
                } else {
                    r->send(400, "application/json",
                        "{\"error\":\"days array required\"}");
                    return;
                }
                tasks[count].start = start;
                tasks[count].duration = duration;
                count++;
            }
            if (!ble_manager::write_schedule(tasks, count)) {
                r->send(503, "application/json",
                    "{\"error\":\"failed to write schedule\"}");
                return;
            }
            r->send(200, "application/json", "{\"ok\":true}");
        });

        // ---- API: pair mower — save MAC + PIN and trigger connect ----
        server.on("/api/mower/pair", HTTP_POST, [](AsyncWebServerRequest* r){ if (!guard(r)) return;
            const AsyncWebParameter* pm = r->getParam("mac", true);
            const AsyncWebParameter* pp = r->getParam("pin", true);
            if (!pm || pm->value().isEmpty()) {
                r->send(400, "application/json", "{\"error\":\"mac required\"}");
                return;
            }
            uint32_t pin = pp ? (uint32_t)pp->value().toInt() : 1234;
            ble_manager::set_target(pm->value().c_str(), pin);
            debug_log::write(debug_log::INFO, SRC,
                "pair request mac=%s pin=%u", pm->value().c_str(), (unsigned)pin);
            r->send(200, "application/json", "{\"ok\":true}");
        });

        // ---- API: disconnect mower ----
        server.on("/api/mower/disconnect", HTTP_POST, [](AsyncWebServerRequest* r){ if (!guard(r)) return;
            ble_manager::disconnect_target();
            r->send(200, "application/json", "{\"ok\":true}");
        });

        // ---- API: save WiFi credentials ----
        server.on("/api/wifi", HTTP_POST, [](AsyncWebServerRequest* r){ if (!guard(r)) return;
            const AsyncWebParameter* ps = r->getParam("ssid", true);
            const AsyncWebParameter* pp = r->getParam("psk",  true);
            String ssid = ps ? ps->value() : "";
            if (ssid.isEmpty()) {
                r->send(400, "application/json", "{\"error\":\"ssid required\"}");
                return;
            }
            settings::set_wifi_ssid(ssid);
            // Blank password = keep the stored one (so re-saving SSID alone
            // doesn't wipe a working PSK). Matches the masked UI placeholder.
            String psk = pp ? pp->value() : "";
            if (!psk.isEmpty()) settings::set_wifi_psk(psk);
            debug_log::write(debug_log::INFO, SRC,
                "WiFi creds saved ssid=%s — restarting", ssid.c_str());
            r->send(200, "application/json", "{\"ok\":true}");
            schedule_restart(300);
        });

        // ---- API: update admin credentials ----
        server.on("/api/admin", HTTP_POST, [](AsyncWebServerRequest* r){ if (!guard(r)) return;
            const AsyncWebParameter* pu = r->getParam("user",     true);
            const AsyncWebParameter* pp = r->getParam("new_pass", true);
            String new_pass = pp ? pp->value() : "";
            if (new_pass.isEmpty()) {
                r->send(400, "application/json", "{\"error\":\"new_pass required\"}");
                return;
            }
            if (pu && !pu->value().isEmpty())
                settings::set_admin_user(pu->value());
            settings::set_admin_pass(new_pass);
            debug_log::write(debug_log::INFO, SRC, "admin credentials updated");
            r->send(200, "application/json", "{\"ok\":true}");
        });

        // ---- API: OTA firmware upload ----
        server.on("/api/ota", HTTP_POST,
            [](AsyncWebServerRequest* req) {
                bool ok = !Update.hasError();
                req->send(200, "application/json",
                    ok ? "{\"ok\":true}" : "{\"error\":\"flash failed\"}");
                if (ok) {
                    debug_log::write(debug_log::INFO, SRC, "OTA flash OK — restarting");
                    // Mark post-OTA so the next boot forces one extra clean
                    // reboot before BLE init — NimBLE panics on the first
                    // boot after an OTA soft-reset (PANIC confirmed in field).
                    settings::set_post_ota(1);
                    schedule_restart(1000);
                } else {
                    debug_log::write(debug_log::ERROR, SRC,
                        "OTA flash error %u", Update.getError());
                }
            },
            [](AsyncWebServerRequest* req, const String& filename,
               size_t index, uint8_t* data, size_t len, bool final) {
                if (index == 0) {
                    debug_log::write(debug_log::INFO, SRC,
                        "OTA upload start: %s", filename.c_str());
                    if (!Update.begin(UPDATE_SIZE_UNKNOWN))
                        debug_log::write(debug_log::ERROR, SRC, "Update.begin failed");
                }
                if (len && Update.write(data, len) != len)
                    debug_log::write(debug_log::ERROR, SRC, "Update.write short");
                if (final) {
                    if (!Update.end(true))
                        debug_log::write(debug_log::ERROR, SRC,
                            "Update.end failed %u", Update.getError());
                    else
                        debug_log::write(debug_log::INFO, SRC,
                            "OTA received %u bytes", index + len);
                }
            }
        );

        // (FS-OTA upload route removed — the UI is embedded in the firmware,
        //  so a single firmware.bin is the only upload needed.)

        // ---- SSE: debug log stream ----
        // On connect, replay buffered entries the client hasn't seen yet.
        _sse.onConnect([](AsyncEventSourceClient* client) {
            struct Ctx { AsyncEventSourceClient* c; uint32_t last_id; };
            Ctx* ctx = new Ctx{client, client->lastId()};
            // Bounded tail only — replaying the whole 256-entry ring into a
            // just-connected client overflows the AsyncEventSource queue.
            debug_log::visit_all([](const char* json, uint32_t id, void* p) {
                Ctx* cx = (Ctx*)p;
                if (id > cx->last_id)
                    cx->c->send(json, "log", id, 500);
            }, ctx, SSE_REPLAY_MAX);
            delete ctx;
            // NB: no debug_log::write() here — it would re-enter sse_broadcast
            // from inside the AsyncTCP onConnect callback (reentrant SSE use).
        });
        server.addHandler(&_sse);

        // ---- 404 ----
        server.onNotFound([](AsyncWebServerRequest* r){
            if (wifi_manager::in_portal_mode()) { r->redirect("/"); return; }
            r->send(404, "text/plain", "Not found");
        });

        server.begin();
        debug_log::write(debug_log::INFO, SRC, "HTTP server started on port 80");

        WiFi.scanNetworks(/*async=*/true);
    }

    void loop() {
        ArduinoOTA.handle();
        sse_drain(); // single-task owner of _sse — see SseMsg note above
    }
}
