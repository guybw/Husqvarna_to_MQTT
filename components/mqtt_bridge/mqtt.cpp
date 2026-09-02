// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native ESP-IDF MQTT bridge, ported from src/mqtt.cpp (PubSubClient ->
// esp-mqtt). esp-mqtt owns its own background task and auto-reconnects, so the
// manual RECONNECT_MS retry loop is gone; connection lifecycle lives in the
// event handler. The HA discovery payloads and the state JSON are byte-for-byte
// identical to the Arduino reference.
#include "mqtt.h"
#include "settings.h"
#include "debug_log.h"
#include "wifi_manager.h"
#include "ble_manager.h"
#include "automower_protocol.h"
#include "mqtt_client.h"
#include "esp_timer.h"
#include <ArduinoJson.h>
#include <cstring>
#include <cstdio>
#include <string>

#define SRC "mqtt"

namespace mqtt {

    static esp_mqtt_client_handle_t _client = nullptr;

    static char     _base[40]   = {};   // "flymo/<id>"
    static char     _t_state[56]= {};   // "<base>/state"
    static char     _t_avail[64]= {};   // "<base>/availability"
    static char     _t_cmd[56]  = {};   // "<base>/cmd"
    static char     _t_sched[64]= {};   // "<base>/schedule"
    static char     _id[16]     = {};   // mac without colons
    static uint32_t _last_pub   = 0;
    static bool     _configured = false;
    static bool     _connected  = false;
    static bool     _ever_connected = false;  // we've reached the broker at least once
    static bool     _wifi_was_up    = false;  // edge-detect WiFi regain
    static uint32_t _wifi_back_at   = 0;      // ms of the last WiFi down→up edge (0 = handled)

    // Broker host/user/pass strings must outlive the client (esp-mqtt keeps
    // pointers into the config). Keep them in file-static std::strings.
    static std::string _host;
    static std::string _user;
    static std::string _pass;

    static constexpr uint32_t PUBLISH_MS = 10000;
    // After WiFi comes back, wait this long for esp-mqtt to reconnect on its own
    // before forcing it. Covers the normal reconnect; only a wedged client (dead
    // half-open socket, or a new DHCP IP after a router reboot) needs the nudge.
    static constexpr uint32_t MQTT_WIFI_REGAIN_GRACE_MS = 15000;

    static uint32_t now_ms() { return (uint32_t)(esp_timer_get_time() / 1000); }

    static const char* conn_str(ble_manager::ConnState s) {
        switch (s) {
            case ble_manager::ConnState::IDLE:          return "idle";
            case ble_manager::ConnState::CONNECTING:    return "connecting";
            case ble_manager::ConnState::HANDSHAKING:   return "handshaking";
            case ble_manager::ConnState::AUTHENTICATED: return "authenticated";
            case ble_manager::ConnState::DORMANT:       return "dormant";
            default:                                    return "error";
        }
    }

    // Command topic handler: "mow" | "park" | "pause" → queue_command;
    // "wake" → force_wake. Anything else ignored.
    static void on_message(const uint8_t* payload, unsigned int len) {
        char cmd[16] = {};
        uint32_t secs = 0;
        bool parsed_json = false;
        size_t offset = 0;
        while (offset < len && (payload[offset] == ' ' || payload[offset] == '\t' ||
               payload[offset] == '\r' || payload[offset] == '\n')) {
            offset++;
        }
        if (offset < len && payload[offset] == '{') {
            DynamicJsonDocument doc(256);
            auto err = deserializeJson(doc, payload, len);
            if (!err) {
                const char* action = doc["action"];
                if (action && action[0] != '\0') {
                    snprintf(cmd, sizeof(cmd), "%s", action);
                    if (doc.containsKey("duration")) {
                        secs = doc["duration"].as<uint32_t>();
                    } else if (doc.containsKey("secs")) {
                        secs = doc["secs"].as<uint32_t>();
                    }
                    parsed_json = true;
                }
            } else {
                debug_log::write(debug_log::WARN, SRC,
                    "invalid MQTT JSON cmd: %s", err.c_str());
            }
        }

        if (!parsed_json) {
            size_t n = len < sizeof(cmd) - 1 ? len : sizeof(cmd) - 1;
            memcpy(cmd, payload, n);
            cmd[n] = '\0';
            // trim trailing CR/LF/space
            while (n > 0 && (cmd[n-1] == '\r' || cmd[n-1] == '\n' || cmd[n-1] == ' '))
                cmd[--n] = '\0';
        }

        debug_log::write(debug_log::INFO, SRC, "cmd via MQTT: '%s'%s%s",
            cmd,
            parsed_json ? "" : "",
            secs ? " (duration)" : "");
        if (strcmp(cmd, "wake") == 0) {
            ble_manager::force_wake();
        } else if (!ble_manager::queue_command(cmd, secs)) {
            debug_log::write(debug_log::WARN, SRC,
                "unknown MQTT cmd '%s' (use mow|park|park_indefinite|resume|pause|"
                "clear_error|wake or JSON action)", cmd);
        }
    }

    // ── HA discovery helpers ───────────────────────────────────────────────
    // Adds the shared HA device block so all entities group under one device.
    static void add_device(JsonDocument& doc) {
        JsonObject dev = doc["dev"].to<JsonObject>();
        char dev_id[24];
        snprintf(dev_id, sizeof(dev_id), "flymo_%s", _id);
        dev["ids"][0] = dev_id;
        dev["name"]   = "Flymo Mower";
        dev["mf"]     = "Flymo";
        dev["mdl"]    = "EasiLife GO 400";
    }

    static void pub_cfg(const char* comp, const char* obj, JsonDocument& doc) {
        add_device(doc);
        char topic[96];
        snprintf(topic, sizeof(topic),
            "homeassistant/%s/flymo_%s/%s/config", comp, _id, obj);
        char payload[768];
        size_t n = serializeJson(doc, payload, sizeof(payload));
        esp_mqtt_client_publish(_client, topic, payload, n, 0, true);
    }

    // Delete a previously-published entity: an empty retained payload to its
    // discovery topic makes HA remove it. Call for sensors we used to publish
    // but dropped, so they don't linger as ghost "Unavailable" entities.
    static void unpub_cfg(const char* comp, const char* obj) {
        char topic[96];
        snprintf(topic, sizeof(topic),
            "homeassistant/%s/flymo_%s/%s/config", comp, _id, obj);
        esp_mqtt_client_publish(_client, topic, "", 0, 0, true);
    }

    static void pub_sensor(const char* obj, const char* name,
                           const char* val_tpl, const char* unit,
                           const char* dev_cla, const char* ent_cat = nullptr) {
        DynamicJsonDocument doc(768);
        doc["name"]    = name;
        char uid[40];
        snprintf(uid, sizeof(uid), "flymo_%s_%s", _id, obj);
        doc["uniq_id"] = uid;
        doc["stat_t"]  = _t_state;
        doc["val_tpl"] = val_tpl;
        doc["avty_t"]  = _t_avail;
        if (unit && *unit)       doc["unit_of_meas"] = unit;
        if (dev_cla && *dev_cla) doc["dev_cla"]      = dev_cla;
        if (ent_cat && *ent_cat) doc["ent_cat"]      = ent_cat;
        pub_cfg("sensor", obj, doc);
    }

    static void pub_button(const char* obj, const char* name,
                           const char* press) {
        DynamicJsonDocument doc(768);
        doc["name"]    = name;
        char uid[40];
        snprintf(uid, sizeof(uid), "flymo_%s_%s", _id, obj);
        doc["uniq_id"]      = uid;
        doc["cmd_t"]        = _t_cmd;
        doc["pl_prs"]       = press;
        doc["avty_t"]       = _t_avail;
        pub_cfg("button", obj, doc);
    }

    static void pub_binary(const char* obj, const char* name,
                           const char* val_tpl, const char* dev_cla,
                           const char* ent_cat = nullptr) {
        DynamicJsonDocument doc(768);
        doc["name"] = name;
        char uid[40];
        snprintf(uid, sizeof(uid), "flymo_%s_%s", _id, obj);
        doc["uniq_id"] = uid;
        doc["stat_t"]  = _t_state;
        doc["val_tpl"] = val_tpl;
        doc["pl_on"]   = "ON";
        doc["pl_off"]  = "OFF";
        doc["avty_t"]  = _t_avail;
        if (dev_cla && *dev_cla) doc["dev_cla"] = dev_cla;
        if (ent_cat && *ent_cat) doc["ent_cat"] = ent_cat;
        pub_cfg("binary_sensor", obj, doc);
    }

    static void pub_switch(const char* obj, const char* name,
                           const char* val_tpl, const char* pl_on,
                           const char* pl_off, const char* ent_cat = nullptr) {
        DynamicJsonDocument doc(768);
        doc["name"] = name;
        char uid[40];
        snprintf(uid, sizeof(uid), "flymo_%s_%s", _id, obj);
        doc["uniq_id"] = uid;
        doc["cmd_t"]   = _t_cmd;
        doc["stat_t"]  = _t_state;
        doc["val_tpl"] = val_tpl;
        doc["pl_on"]   = pl_on;
        doc["pl_off"]  = pl_off;
        doc["stat_on"] = "ON";
        doc["stat_off"]= "OFF";
        doc["avty_t"]  = _t_avail;
        if (ent_cat && *ent_cat) doc["ent_cat"] = ent_cat;
        pub_cfg("switch", obj, doc);
    }

    // Duration sensor formatted as "Xd Yh Zm" (the app's style) instead of
    // raw seconds. String state (no unit/device_class).
    static void pub_dur(const char* obj, const char* name, const char* key,
                        const char* ent_cat = nullptr) {
        DynamicJsonDocument doc(768);
        doc["name"] = name;
        char uid[40];
        snprintf(uid, sizeof(uid), "flymo_%s_%s", _id, obj);
        doc["uniq_id"] = uid;
        doc["stat_t"]  = _t_state;
        char tpl[200];
        snprintf(tpl, sizeof(tpl),
            "{%% set s=value_json.%s %%}{%% if s is none %%}Unknown{%% else %%}"
            "{{ '%%dd %%dh %%dm' %% (s//86400,(s%%86400)//3600,(s%%3600)//60) }}"
            "{%% endif %%}", key);
        doc["val_tpl"] = tpl;
        doc["avty_t"]  = _t_avail;
        if (ent_cat && *ent_cat) doc["ent_cat"] = ent_cat;
        pub_cfg("sensor", obj, doc);
    }

    // Select with a fixed 4-option list (Off/Low/Medium/High — LawnSense).
    static void pub_select4(const char* obj, const char* name,
                            const char* o0, const char* o1,
                            const char* o2, const char* o3,
                            const char* val_tpl, const char* cmd_tpl,
                            const char* ent_cat = nullptr) {
        DynamicJsonDocument doc(768);
        doc["name"] = name;
        char uid[40];
        snprintf(uid, sizeof(uid), "flymo_%s_%s", _id, obj);
        doc["uniq_id"] = uid;
        doc["cmd_t"]   = _t_cmd;
        doc["stat_t"]  = _t_state;
        doc["val_tpl"] = val_tpl;
        doc["cmd_tpl"] = cmd_tpl;
        JsonArray opt = doc["options"].to<JsonArray>();
        opt.add(o0); opt.add(o1);
        if (o2) opt.add(o2);   // o2/o3 may be null for a 2- or 3-option select
        if (o3) opt.add(o3);
        doc["avty_t"]  = _t_avail;
        if (ent_cat && *ent_cat) doc["ent_cat"] = ent_cat;
        pub_cfg("select", obj, doc);
    }

    // HA "number" entity (slider/box). cmd_tpl formats the chosen value into a
    // command for our _t_cmd topic; val_tpl extracts the current value.
    static void pub_number(const char* obj, const char* name,
                           const char* val_tpl, const char* cmd_tpl,
                           int mn, int mx, int step, const char* unit,
                           const char* ent_cat = nullptr) {
        DynamicJsonDocument doc(768);
        doc["name"] = name;
        char uid[40];
        snprintf(uid, sizeof(uid), "flymo_%s_%s", _id, obj);
        doc["uniq_id"] = uid;
        doc["cmd_t"]   = _t_cmd;
        doc["stat_t"]  = _t_state;
        doc["val_tpl"] = val_tpl;
        doc["cmd_tpl"] = cmd_tpl;
        doc["min"]     = mn;
        doc["max"]     = mx;
        doc["step"]    = step;
        if (unit && *unit)       doc["unit_of_meas"] = unit;
        if (ent_cat && *ent_cat) doc["ent_cat"]      = ent_cat;
        pub_cfg("number", obj, doc);
    }

    // Publish HA MQTT-discovery configs (retained). Called once per connect.
    // Entities are grouped by HA entity_category: primary (no category) for the
    // at-a-glance status + the mow/park/pause/wake buttons; "config" for the
    // mower settings (switches/selects); "diagnostic" for the deeper telemetry
    // so it tucks under the device's Diagnostic section instead of cluttering
    // the main card.
    static void pub_discovery() {
        // ── Primary: at-a-glance status ────────────────────────────────────
        pub_sensor("battery", "Battery",
            "{{ value_json.battery }}", "%", "battery");
        pub_sensor("state", "Status",
            "{% set m={0:'Off',1:'Wait PIN',2:'Stopped',3:'Fatal error',"
            "4:'Pending start',5:'Paused',6:'Mowing',7:'Restricted',"
            "8:'Error'} %}{{ m.get(value_json.state|int(-1),'Unknown') "
            "if value_json.state is not none else 'Unknown' }}", "", "");
        pub_sensor("activity", "Activity",
            "{% set a={0:'None',1:'Charging',2:'Leaving',3:'Mowing',"
            "4:'Returning',5:'Parked',6:'Stopped in garden'} %}"
            "{{ a.get(value_json.activity|int(-1),'Unknown') "
            "if value_json.activity is not none else 'Unknown' }}", "", "");
        pub_binary("charging", "Charging",
            "{{ 'ON' if value_json.charging else 'OFF' }}", "battery_charging");
        // RestrictedReason enum recovered from the Flymo APK:
        // 0 None,1 Park override,2 Week schedule,3 Sensor,4 Daily limit,
        // 5 Frost sensor,6 Missions complete.
        pub_sensor("restriction", "Restriction reason",
            "{% set r={0:'None',1:'Park override',2:'Week schedule',"
            "3:'Sensor',4:'Daily limit',5:'Frost',6:'Missions complete'} %}"
            "{{ r.get(value_json.restriction|int(-1),'Unknown') "
            "if value_json.restriction is not none else 'Unknown' }}", "", "");
        pub_sensor("next_start", "Next start",
            "{% set s=value_json.next_start %}"
            "{% if s is none or s==0 %}—{% else %}"
            "{{ s | timestamp_local }}{% endif %}", "", "");
        pub_binary("collision", "Collision",
            "{{ 'ON' if value_json.collision else 'OFF' }}", "problem");
        // Frost hold: mowing suspended by the mower's frost sensor
        // (state RESTRICTED + RestrictionReason == 5). Uses data we already
        // poll (GetRestrictionReason 4658:0) — no extra BLE command.
        pub_binary("frost", "Frost hold",
            "{{ 'ON' if value_json.restriction == 5 else 'OFF' }}", "cold");

        // ── Controls: buttons (primary) + settings (config category) ───────
        pub_button("wake",  "Wake",  "wake");
        pub_button("mow",   "Mow",   "mow");
        pub_button("park",  "Park (until next schedule starts)", "park");
        pub_button("park_hold", "Park (until further notice)", "park_indefinite");
        pub_button("resume", "Resume schedule", "resume");
        // Option 3 (park for a custom duration, overriding the schedule) is a
        // value+action, so it doesn't fit a static button. HA automations can do
        // it by publishing JSON to the cmd topic, e.g.
        //   {"action":"park","duration":10800}   → park for 3 h then resume.
        pub_button("pause", "Pause", "pause");
        // Acknowledge a latched fatal error (ConfirmError 4586:9) — only useful
        // once the underlying problem (e.g. Upside down) is actually fixed.
        pub_button("clear_error", "Clear error", "clear_error");
        // FrostSense control switch (writes SetEnabled 5370:2 to the mower).
        pub_switch("frost_switch", "FrostSense",
            "{{ 'ON' if value_json.frost_enabled else 'OFF' }}",
            "frost_on", "frost_off", "config");
        // Avoid garage (ChargingStation SetMowerHouseInstalled 4692:3) and
        // LawnSense (Autotimer): on/off + Low/Med/High sensitivity in one
        // Off/Low/Medium/High select.
        pub_switch("garage", "Avoid garage",
            "{{ 'ON' if value_json.garage else 'OFF' }}",
            "garage_on", "garage_off", "config");
        pub_select4("lawnsense", "LawnSense",
            "Off", "Low", "Medium", "High",
            "{{ value_json.lawn_opt if value_json.lawn_opt is not none "
            "else 'None' }}",
            "{{ {'Off':'lawn_off','Low':'lawn_low','Medium':'lawn_med',"
            "'High':'lawn_high'}[value] }}", "config");
        // Drive-past-wire (4712:0/1): front overrun distance past the boundary
        // wire before turning back (default 32 cm). Reduce to cut down edge
        // over-mowing / perimeter collisions.
        pub_number("drive_past", "Drive past wire",
            "{{ value_json.drive_past }}",
            "{\"action\":\"drivepast\",\"duration\":{{ value }}}",
            0, 50, 1, "cm", "config");
        // Collision responsiveness (4166:11/12): how sensitively the mower
        // reacts to bumps (Low/Medium/High). Fall back to the literal "None"
        // (HA's PAYLOAD_NONE) when the mower hasn't reported it — a model that
        // lacks 4166/11, or before the first read. Anything not in the option
        // list makes HA's mqtt.select log "Invalid option" on every publish.
        pub_select4("collision_resp", "Collision sensitivity",
            "Low", "Medium", "High", nullptr,
            "{{ value_json.collision_opt if value_json.collision_opt is not none "
            "else 'None' }}",
            "{{ {'Low':'collision_low','Medium':'collision_med',"
            "'High':'collision_high'}[value] }}", "config");

        // ── Diagnostics (tucked under the device's Diagnostic section) ──────
        pub_sensor("connection", "Bridge connection",
            "{{ value_json.conn }}", "", "", "diagnostic");
        pub_dur("charge_left", "Charge time remaining", "charge_left", "diagnostic");
        pub_sensor("error", "Error code",
            "{{ value_json.error_text if value_json.error_text is not none else 'Unknown' }}",
            "", "", "diagnostic");
        pub_sensor("batt_temp", "Battery temperature",
            "{{ value_json.batt_temp }}", "°C", "temperature", "diagnostic");
        pub_sensor("pitch", "Pitch angle",
            "{{ value_json.pitch }}", "°", "", "diagnostic");
        pub_sensor("roll", "Roll angle",
            "{{ value_json.roll }}", "°", "", "diagnostic");
        pub_binary("lift", "Lifted",
            "{{ 'ON' if value_json.lift else 'OFF' }}", "problem", "diagnostic");
        pub_sensor("power_mode", "Power mode",
            "{{ value_json.power_mode if value_json.power_mode is not none "
            "else 'Unknown' }}", "", "", "diagnostic");
        // FrostSense (read-only, GetAllSettings 5370:8): is frost protection
        // available on this mower and is it currently switched on.
        pub_binary("frost_available", "FrostSense available",
            "{{ 'ON' if value_json.frost_avail else 'OFF' }}", "", "diagnostic");
        pub_binary("frost_enabled", "FrostSense enabled",
            "{{ 'ON' if value_json.frost_enabled else 'OFF' }}", "", "diagnostic");
        // Battery voltage (4106:1) and charging-wire loop signals (4462:13/14).
        pub_sensor("batt_voltage", "Battery voltage",
            "{{ value_json.batt_voltage }}", "V", "voltage", "diagnostic");
        pub_sensor("loop_strength", "Loop signal strength",
            "{{ value_json.loop_strength }}", "%", "", "diagnostic");
        pub_sensor("loop_a", "A-loop signal",
            "{{ value_json.loop_a }}", "", "", "diagnostic");
        pub_sensor("loop_f", "F-loop signal",
            "{{ value_json.loop_f }}", "", "", "diagnostic");
        pub_sensor("loop_guide", "Guide-wire signal",
            "{{ value_json.loop_guide }}", "", "", "diagnostic");
        pub_dur("run_time",    "Total running time",   "run_time",    "diagnostic");
        pub_dur("cut_time",    "Total cutting time",   "cut_time",    "diagnostic");
        pub_dur("charge_time", "Total charging time",  "charge_time", "diagnostic");
        pub_dur("search_time", "Total searching time", "search_time", "diagnostic");
        pub_sensor("collisions", "Lifetime collisions",
            "{{ value_json.collisions }}", "", "", "diagnostic");
        pub_sensor("charge_cycles", "Charging cycles",
            "{{ value_json.charge_cycles }}", "", "", "diagnostic");
        pub_dur("blade_time", "Blade usage time", "blade_time", "diagnostic");
        // Mower schedule (read once per connected session). A diagnostic sensor
        // whose state is a short summary and whose attributes hold the task list.
        {
            DynamicJsonDocument doc(768);
            doc["name"] = "Schedule";
            char uid[40];
            snprintf(uid, sizeof(uid), "flymo_%s_schedule", _id);
            doc["uniq_id"]      = uid;
            doc["stat_t"]       = _t_sched;
            doc["val_tpl"]      = "{{ value_json.summary }}";
            doc["json_attr_t"]  = _t_sched;
            doc["json_attr_tpl"]= "{{ {'tasks': value_json.tasks} | tojson }}";
            doc["avty_t"]       = _t_avail;
            doc["ent_cat"]      = "diagnostic";
            pub_cfg("sensor", "schedule", doc);
        }

        // Remove entities published by earlier firmware but since dropped as
        // unsupported on the GO 400 (empty retained payload = HA deletes it).
        unpub_cfg("sensor",        "board_temp");  // 20:4 mowertemp — unsupported
        unpub_cfg("binary_sensor", "upsidedown");  // 20:4 — unsupported
        unpub_cfg("sensor",        "theft");       // 4736:21 — unsupported
        debug_log::write(debug_log::INFO, SRC, "HA discovery published");
    }

    static void publish_state() {
        ble_manager::MowerStatus st = ble_manager::get_mower_status();
        char ms[8], ma[8], mb[8], mch[8], mcl[16], mer[16], mrs[8], mns[16];
        if (st.mower_state    >= 0) snprintf(ms, sizeof(ms), "%d", (int)st.mower_state);    else strcpy(ms, "null");
        if (st.mower_activity >= 0) snprintf(ma, sizeof(ma), "%d", (int)st.mower_activity); else strcpy(ma, "null");
        if (st.mower_battery  >= 0) snprintf(mb, sizeof(mb), "%d", (int)st.mower_battery);  else strcpy(mb, "null");
        if (st.mower_charging >= 0) strcpy(mch, st.mower_charging ? "true" : "false");      else strcpy(mch, "null");
        if (st.mower_charge_left >= 0) snprintf(mcl, sizeof(mcl), "%ld", (long)st.mower_charge_left); else strcpy(mcl, "null");
        if (st.mower_error      >= 0) snprintf(mer, sizeof(mer), "%ld", (long)st.mower_error);        else strcpy(mer, "null");
        char mtx[80];
        if (st.mower_error >= 0) snprintf(mtx, sizeof(mtx), "\"%s\"", automower::mower_error_str(st.mower_error)); else strcpy(mtx, "null");
        if (st.mower_restriction>= 0) snprintf(mrs, sizeof(mrs), "%d", (int)st.mower_restriction);    else strcpy(mrs, "null");
        if (st.mower_next_start >= 0) snprintf(mns, sizeof(mns), "%ld", (long)st.mower_next_start);   else strcpy(mns, "null");

        // Outdoor/diagnostic sensors. INT16_MIN = unknown for signed
        // temps/angles; -1 = unknown for the 0/1 flags and counters.
        char bt[8], pa[10], ra[10], pm[8];
        char co[8], li[8], fa[8], fe[8];
        char bv[12], ls[8], la[10], lf[10], lg[10];
        char gg[8], lav[8], lo[10];
        char s_run[16], s_cut[16], s_chg[16], s_sea[16], s_col[16], s_cyc[16], s_bld[16];
        if (st.mower_batt_temp  != INT16_MIN) snprintf(bt, sizeof(bt), "%d", (int)st.mower_batt_temp);  else strcpy(bt, "null");
        // Pitch/roll raw are deci-degrees (the app divides by 10) → °, 1 dp.
        if (st.mower_pitch      != INT16_MIN) snprintf(pa, sizeof(pa), "%.1f", st.mower_pitch / 10.0); else strcpy(pa, "null");
        if (st.mower_roll       != INT16_MIN) snprintf(ra, sizeof(ra), "%.1f", st.mower_roll  / 10.0); else strcpy(ra, "null");
        if (st.mower_power_mode >= 0) snprintf(pm, sizeof(pm), "%d", (int)st.mower_power_mode);         else strcpy(pm, "null");
        if (st.mower_collision  >= 0) strcpy(co, st.mower_collision  ? "true" : "false");               else strcpy(co, "null");
        if (st.mower_lift       >= 0) strcpy(li, st.mower_lift       ? "true" : "false");               else strcpy(li, "null");
        if (st.mower_frost_avail   >= 0) strcpy(fa, st.mower_frost_avail   ? "true" : "false");         else strcpy(fa, "null");
        if (st.mower_frost_enabled >= 0) strcpy(fe, st.mower_frost_enabled ? "true" : "false");         else strcpy(fe, "null");
        if (st.mower_batt_mv >= 0) snprintf(bv, sizeof(bv), "%.2f", st.mower_batt_mv / 1000.0); else strcpy(bv, "null");
        if (st.loop_strength >= 0) snprintf(ls, sizeof(ls), "%d", (int)st.loop_strength);       else strcpy(ls, "null");
        if (st.loop_a     != INT16_MIN) snprintf(la, sizeof(la), "%d", (int)st.loop_a);         else strcpy(la, "null");
        if (st.loop_f     != INT16_MIN) snprintf(lf, sizeof(lf), "%d", (int)st.loop_f);         else strcpy(lf, "null");
        if (st.loop_guide != INT16_MIN) snprintf(lg, sizeof(lg), "%d", (int)st.loop_guide);     else strcpy(lg, "null");
        if (st.mower_garage >= 0) strcpy(gg, st.mower_garage ? "true" : "false");               else strcpy(gg, "null");
        if (st.lawn_avail   >= 0) strcpy(lav, st.lawn_avail  ? "true" : "false");               else strcpy(lav, "null");
        // LawnSense select option: Off if disabled, else Low/Med/High by sens.
        if (st.lawn_enabled < 0)        strcpy(lo, "null");
        else if (st.lawn_enabled == 0)  strcpy(lo, "\"Off\"");
        else if (st.lawn_sens == 1)     strcpy(lo, "\"Low\"");
        else if (st.lawn_sens == 2)     strcpy(lo, "\"Medium\"");
        else if (st.lawn_sens == 3)     strcpy(lo, "\"High\"");
        else                            strcpy(lo, "null");  // not an option → HA select warns; let val_tpl map to "None"
        if (st.stat_running    >= 0) snprintf(s_run, sizeof(s_run), "%ld", (long)st.stat_running);    else strcpy(s_run, "null");
        if (st.stat_cutting    >= 0) snprintf(s_cut, sizeof(s_cut), "%ld", (long)st.stat_cutting);    else strcpy(s_cut, "null");
        if (st.stat_charging   >= 0) snprintf(s_chg, sizeof(s_chg), "%ld", (long)st.stat_charging);   else strcpy(s_chg, "null");
        if (st.stat_searching  >= 0) snprintf(s_sea, sizeof(s_sea), "%ld", (long)st.stat_searching);  else strcpy(s_sea, "null");
        if (st.stat_collisions >= 0) snprintf(s_col, sizeof(s_col), "%ld", (long)st.stat_collisions); else strcpy(s_col, "null");
        if (st.stat_cycles     >= 0) snprintf(s_cyc, sizeof(s_cyc), "%ld", (long)st.stat_cycles);     else strcpy(s_cyc, "null");
        if (st.stat_blade      >= 0) snprintf(s_bld, sizeof(s_bld), "%ld", (long)st.stat_blade);      else strcpy(s_bld, "null");

        char dpw[8], cro[12];
        if (st.drive_past >= 0) snprintf(dpw, sizeof(dpw), "%d", (int)st.drive_past); else strcpy(dpw, "null");
        if      (st.collision_resp == 0) strcpy(cro, "\"Low\"");
        else if (st.collision_resp == 1) strcpy(cro, "\"Medium\"");
        else if (st.collision_resp == 2) strcpy(cro, "\"High\"");
        else                              strcpy(cro, "null");

        char buf[1200];
        int n = snprintf(buf, sizeof(buf),
            "{\"conn\":\"%s\",\"state\":%s,\"activity\":%s,\"battery\":%s,"
            "\"charging\":%s,\"charge_left\":%s,\"error\":%s,\"error_text\":%s,"
            "\"restriction\":%s,\"next_start\":%s,"
            "\"batt_temp\":%s,\"pitch\":%s,\"roll\":%s,"
            "\"collision\":%s,\"lift\":%s,"
            "\"power_mode\":%s,\"frost_avail\":%s,\"frost_enabled\":%s,"
            "\"batt_voltage\":%s,\"loop_strength\":%s,"
            "\"loop_a\":%s,\"loop_f\":%s,\"loop_guide\":%s,"
            "\"garage\":%s,\"lawn_avail\":%s,\"lawn_opt\":%s,"
            "\"run_time\":%s,\"cut_time\":%s,\"charge_time\":%s,"
            "\"search_time\":%s,\"collisions\":%s,\"charge_cycles\":%s,"
            "\"blade_time\":%s,\"drive_past\":%s,\"collision_opt\":%s}",
            conn_str(st.state), ms, ma, mb, mch, mcl, mer, mtx, mrs, mns,
            bt, pa, ra, co, li, pm, fa, fe,
            bv, ls, la, lf, lg,
            gg, lav, lo,
            s_run, s_cut, s_chg, s_sea, s_col, s_cyc, s_bld, dpw, cro);
        if (n < 0) return;
        esp_mqtt_client_publish(_client, _t_state, buf, n, 0, true);
    }

    // Publish the mower's cached schedule (read once per connected session by
    // ble_manager). Retained so HA always has the last-known schedule even while
    // the mower sleeps. Payload: {count, summary, tasks:[{start,duration_min,days}]}.
    static void publish_schedule() {
        static const char* DOW[7] = {"Mon","Tue","Wed","Thu","Fri","Sat","Sun"};
        ble_manager::ScheduleTask tasks[16];
        uint32_t count = 0;
        bool have = ble_manager::get_schedule_cache(tasks, 16, &count);

        DynamicJsonDocument doc(2048);
        doc["count"] = have ? (int)count : 0;
        char summ[24];
        if (!have)            strcpy(summ, "Unknown");
        else if (count == 0)  strcpy(summ, "No tasks");
        else snprintf(summ, sizeof(summ), "%u task%s", (unsigned)count,
                      count == 1 ? "" : "s");
        doc["summary"] = summ;
        JsonArray arr = doc.createNestedArray("tasks");
        if (have) {
            for (uint32_t i = 0; i < count; i++) {
                JsonObject t = arr.createNestedObject();
                unsigned hh = (unsigned)((tasks[i].start / 3600) % 24);
                unsigned mm = (unsigned)((tasks[i].start % 3600) / 60);
                char hhmm[8];
                snprintf(hhmm, sizeof(hhmm), "%02u:%02u", hh, mm);
                t["start"]        = hhmm;
                t["duration_min"] = (int)(tasks[i].duration / 60);
                JsonArray d = t.createNestedArray("days");
                for (int k = 0; k < 7; k++) if (tasks[i].use_on[k]) d.add(DOW[k]);
            }
        }
        char payload[1536];
        size_t n = serializeJson(doc, payload, sizeof(payload));
        esp_mqtt_client_publish(_client, _t_sched, payload, n, 0, true);
    }

    // esp-mqtt event handler. Runs on esp-mqtt's own task.
    static void mqtt_event_handler(void* handler_args, esp_event_base_t base,
                                   int32_t event_id, void* event_data) {
        esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)event_data;
        switch ((esp_mqtt_event_id_t)event_id) {
            case MQTT_EVENT_CONNECTED:
                _connected = true;
                _ever_connected = true;
                esp_mqtt_client_publish(_client, _t_avail, "online", 0, 0, true);
                esp_mqtt_client_subscribe(_client, _t_cmd, 0);
                pub_discovery();
                debug_log::write(debug_log::INFO, SRC,
                    "connected to broker (sub %s)", _t_cmd);
                break;
            case MQTT_EVENT_DISCONNECTED:
                _connected = false;
                debug_log::write(debug_log::WARN, SRC, "broker disconnected");
                break;
            case MQTT_EVENT_DATA:
                // Inbound command. Topic is always _t_cmd (our only sub).
                on_message((const uint8_t*)e->data, (unsigned int)e->data_len);
                break;
            case MQTT_EVENT_ERROR:
                debug_log::write(debug_log::WARN, SRC, "mqtt transport error");
                break;
            default:
                break;
        }
    }

    void begin() {
        _host = settings::get_mqtt_host();
        if (_host.empty()) {
            debug_log::write(debug_log::INFO, SRC, "MQTT disabled (no host set)");
            return;
        }
        _configured = true;

        // Topic id = mower MAC without colons (single-mower; stable per device).
        std::string mac = settings::get_mower_mac();
        char id[16] = "unknown";
        if (!mac.empty()) {
            size_t j = 0;
            for (size_t i = 0; i < mac.length() && j < sizeof(id) - 1; i++)
                if (mac[i] != ':') id[j++] = mac[i];
            id[j] = '\0';
        }
        strncpy(_id, id, sizeof(_id) - 1);
        snprintf(_base,    sizeof(_base),    "flymo/%s", id);
        snprintf(_t_state, sizeof(_t_state), "%s/state", _base);
        snprintf(_t_avail, sizeof(_t_avail), "%s/availability", _base);
        snprintf(_t_cmd,   sizeof(_t_cmd),   "%s/cmd", _base);
        snprintf(_t_sched, sizeof(_t_sched), "%s/schedule", _base);

        uint16_t port = settings::get_mqtt_port();
        _user = settings::get_mqtt_user();
        _pass = settings::get_mqtt_pass();

        esp_mqtt_client_config_t cfg = {};
        cfg.broker.address.hostname  = _host.c_str();
        cfg.broker.address.port      = port;
        cfg.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
        // Client id derived from base (matches PubSubClient's connect(_base,...)).
        cfg.credentials.client_id    = _base;
        if (!_user.empty()) cfg.credentials.username                   = _user.c_str();
        if (!_pass.empty()) cfg.credentials.authentication.password    = _pass.c_str();
        // LWT marks us offline on an ungraceful drop, matching the reference.
        cfg.session.last_will.topic  = _t_avail;
        cfg.session.last_will.msg    = "offline";
        cfg.session.last_will.msg_len= 0;       // 0 = strlen("offline")
        cfg.session.last_will.qos    = 0;
        cfg.session.last_will.retain = true;
        // Shorter keepalive (default 120 s) so a dead link — e.g. the AP vanishing
        // on a power cut with no deauth, leaving a half-open socket — is detected
        // and reconnected in ~45 s instead of a few minutes.
        cfg.session.keepalive        = 30;
        // HA discovery payloads exceed esp-mqtt's default 1024 outbox/buffer
        // headroom on bursts; bump the TX buffer like PubSubClient's setBufferSize.
        cfg.buffer.size              = 2048;  // headroom for HA discovery + schedule

        _client = esp_mqtt_client_init(&cfg);
        if (!_client) {
            debug_log::write(debug_log::ERROR, SRC, "esp_mqtt_client_init failed");
            _configured = false;
            return;
        }
        esp_mqtt_client_register_event(_client, MQTT_EVENT_ANY,
                                       mqtt_event_handler, nullptr);
        esp_mqtt_client_start(_client);

        debug_log::write(debug_log::INFO, SRC, "MQTT → %s:%u base=%s",
            _host.c_str(), (unsigned)port, _base);
    }

    void loop() {
        if (!_configured) return;

        // esp-mqtt auto-reconnects on its own, but after a longer WiFi outage it
        // can sit in a long backoff (or on a stale socket) while the STA is
        // already back — the symptom being "WiFi recovered but MQTT stayed
        // offline until a power-cycle". On the WiFi down→up edge, give esp-mqtt a
        // grace period then force one clean reconnect from a known state.
        bool wifi_up = wifi_manager::is_connected();
        if (wifi_up && !_wifi_was_up) _wifi_back_at = now_ms();  // just returned
        _wifi_was_up = wifi_up;
        if (wifi_up && !_connected && _ever_connected && _wifi_back_at &&
            now_ms() - _wifi_back_at >= MQTT_WIFI_REGAIN_GRACE_MS) {
            _wifi_back_at = 0;                                   // one nudge per outage
            debug_log::write(debug_log::WARN, SRC,
                "WiFi back but broker still unreachable — forcing reconnect");
            esp_mqtt_client_reconnect(_client);
        }

        if (!wifi_up) return;
        if (!_connected) return;       // esp-mqtt task handles (re)connect

        uint32_t now = now_ms();
        if (now - _last_pub >= PUBLISH_MS) {
            _last_pub = now;
            publish_state();
            publish_schedule();
        }
    }
}
