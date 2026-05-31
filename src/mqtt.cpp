// SPDX-License-Identifier: GPL-3.0-or-later
#include "mqtt.h"
#include "settings.h"
#include "debug_log.h"
#include "wifi_manager.h"
#include "ble/ble_manager.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#define SRC "mqtt"

namespace mqtt {

    static WiFiClient   _net;
    static PubSubClient _client(_net);

    static char     _base[40]   = {};   // "flymo/<id>"
    static char     _t_state[56]= {};   // "<base>/state"
    static char     _t_avail[64]= {};   // "<base>/availability"
    static char     _t_cmd[56]  = {};   // "<base>/cmd"
    static char     _id[16]     = {};   // mac without colons
    static uint32_t _last_pub   = 0;
    static uint32_t _last_try   = 0;
    static bool     _configured = false;

    static constexpr uint32_t PUBLISH_MS   = 10000;
    static constexpr uint32_t RECONNECT_MS = 5000;

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
    static void on_message(char* topic, byte* payload, unsigned int len) {
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
                "unknown MQTT cmd '%s' (use mow|park|pause|wake or JSON action)", cmd);
        }
    }

    void begin() {
        String host = settings::get_mqtt_host();
        if (host.isEmpty()) {
            debug_log::write(debug_log::INFO, SRC, "MQTT disabled (no host set)");
            return;
        }
        _configured = true;

        // Topic id = mower MAC without colons (single-mower; stable per device).
        String mac = settings::get_mower_mac();
        char id[16] = "unknown";
        if (!mac.isEmpty()) {
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
        _client.setCallback(on_message);
        _client.setBufferSize(1024); // HA discovery payloads exceed default 256

        // If host is a numeric IP, use the IPAddress overload so PubSubClient
        // skips DNS — resolving a literal IP string can fail on ESP32 and
        // return rc=-2 even when the broker is reachable.
        uint16_t port = settings::get_mqtt_port();
        IPAddress ip;
        if (ip.fromString(host)) {
            _client.setServer(ip, port);
            debug_log::write(debug_log::INFO, SRC, "MQTT → %s:%u (ip) base=%s",
                host.c_str(), (unsigned)port, _base);
        } else {
            _client.setServer(host.c_str(), port);
            debug_log::write(debug_log::INFO, SRC, "MQTT → %s:%u (dns) base=%s",
                host.c_str(), (unsigned)port, _base);
        }
    }

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
        _client.publish(topic, (const uint8_t*)payload, n, true);
    }

    // Delete a previously-published entity: an empty retained payload to its
    // discovery topic makes HA remove it. Call for sensors we used to publish
    // but dropped, so they don't linger as ghost "Unavailable" entities.
    static void unpub_cfg(const char* comp, const char* obj) {
        char topic[96];
        snprintf(topic, sizeof(topic),
            "homeassistant/%s/flymo_%s/%s/config", comp, _id, obj);
        _client.publish(topic, (const uint8_t*)"", 0, true);
    }

    static void pub_sensor(const char* obj, const char* name,
                           const char* val_tpl, const char* unit,
                           const char* dev_cla) {
        JsonDocument doc;
        doc["name"]    = name;
        char uid[40];
        snprintf(uid, sizeof(uid), "flymo_%s_%s", _id, obj);
        doc["uniq_id"] = uid;
        doc["stat_t"]  = _t_state;
        doc["val_tpl"] = val_tpl;
        doc["avty_t"]  = _t_avail;
        if (unit && *unit)       doc["unit_of_meas"] = unit;
        if (dev_cla && *dev_cla) doc["dev_cla"]      = dev_cla;
        pub_cfg("sensor", obj, doc);
    }

    static void pub_button(const char* obj, const char* name,
                           const char* press) {
        JsonDocument doc;
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
                           const char* val_tpl, const char* dev_cla) {
        JsonDocument doc;
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
        pub_cfg("binary_sensor", obj, doc);
    }

    static void pub_switch(const char* obj, const char* name,
                           const char* val_tpl, const char* pl_on,
                           const char* pl_off) {
        JsonDocument doc;
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
        pub_cfg("switch", obj, doc);
    }

    // Duration sensor formatted as "Xd Yh Zm" (the app's style) instead of
    // raw seconds. String state (no unit/device_class).
    static void pub_dur(const char* obj, const char* name, const char* key) {
        JsonDocument doc;
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
        pub_cfg("sensor", obj, doc);
    }

    // Select with a fixed 4-option list (Off/Low/Medium/High — LawnSense).
    static void pub_select4(const char* obj, const char* name,
                            const char* o0, const char* o1,
                            const char* o2, const char* o3,
                            const char* val_tpl, const char* cmd_tpl) {
        JsonDocument doc;
        doc["name"] = name;
        char uid[40];
        snprintf(uid, sizeof(uid), "flymo_%s_%s", _id, obj);
        doc["uniq_id"] = uid;
        doc["cmd_t"]   = _t_cmd;
        doc["stat_t"]  = _t_state;
        doc["val_tpl"] = val_tpl;
        doc["cmd_tpl"] = cmd_tpl;
        JsonArray opt = doc["options"].to<JsonArray>();
        opt.add(o0); opt.add(o1); opt.add(o2); opt.add(o3);
        doc["avty_t"]  = _t_avail;
        pub_cfg("select", obj, doc);
    }

    // Publish HA MQTT-discovery configs (retained). Called once per connect.
    static void pub_discovery() {
        pub_sensor("battery", "Battery",
            "{{ value_json.battery }}", "%", "battery");
        pub_sensor("state", "State",
            "{% set m={0:'Off',1:'Wait PIN',2:'Stopped',3:'Fatal error',"
            "4:'Pending start',5:'Paused',6:'Mowing',7:'Restricted',"
            "8:'Error'} %}{{ m.get(value_json.state|int(-1),'Unknown') "
            "if value_json.state is not none else 'Unknown' }}", "", "");
        pub_sensor("activity", "Activity",
            "{% set a={0:'None',1:'Charging',2:'Leaving',3:'Mowing',"
            "4:'Returning',5:'Parked',6:'Stopped in garden'} %}"
            "{{ a.get(value_json.activity|int(-1),'Unknown') "
            "if value_json.activity is not none else 'Unknown' }}", "", "");
        pub_sensor("connection", "Bridge connection",
            "{{ value_json.conn }}", "", "");
        pub_binary("charging", "Charging",
            "{{ 'ON' if value_json.charging else 'OFF' }}", "battery_charging");
        pub_dur("charge_left", "Charge time remaining", "charge_left");
        pub_sensor("error", "Error code",
            "{{ value_json.error if value_json.error is not none else 'Unknown' }}",
            "", "");
        // RestrictedReason enum recovered from the Flymo APK:
        // 0 None,1 Park override,2 Week schedule,3 Sensor,4 Daily limit,
        // 5 Frost sensor,6 Missions complete.
        pub_sensor("restriction", "Restriction reason",
            "{% set r={0:'None',1:'Park override',2:'Week schedule',"
            "3:'Sensor',4:'Daily limit',5:'Frost',6:'Missions complete'} %}"
            "{{ r.get(value_json.restriction|int(-1),'Unknown') "
            "if value_json.restriction is not none else 'Unknown' }}", "", "");
        // Frost hold: mowing suspended by the mower's frost sensor
        // (state RESTRICTED + RestrictionReason == 5). Uses data we already
        // poll (GetRestrictionReason 4658:0) — no extra BLE command.
        pub_binary("frost", "Frost hold",
            "{{ 'ON' if value_json.restriction == 5 else 'OFF' }}", "cold");
        pub_sensor("next_start", "Next start",
            "{% set s=value_json.next_start %}"
            "{% if s is none or s==0 %}—{% else %}"
            "{{ s | timestamp_local }}{% endif %}", "", "");
        // ── Outdoor/diagnostic sensors (APK-recovered, see protocol-notes) ──
        pub_sensor("batt_temp", "Battery temperature",
            "{{ value_json.batt_temp }}", "°C", "temperature");
        pub_sensor("pitch", "Pitch angle",
            "{{ value_json.pitch }}", "°", "");
        pub_sensor("roll", "Roll angle",
            "{{ value_json.roll }}", "°", "");
        pub_binary("collision", "Collision",
            "{{ 'ON' if value_json.collision else 'OFF' }}", "problem");
        pub_binary("lift", "Lifted",
            "{{ 'ON' if value_json.lift else 'OFF' }}", "problem");
        pub_sensor("power_mode", "Power mode",
            "{{ value_json.power_mode if value_json.power_mode is not none "
            "else 'Unknown' }}", "", "");
        // FrostSense (read-only, GetAllSettings 5370:8): is frost protection
        // available on this mower and is it currently switched on.
        pub_binary("frost_available", "FrostSense available",
            "{{ 'ON' if value_json.frost_avail else 'OFF' }}", "");
        pub_binary("frost_enabled", "FrostSense enabled",
            "{{ 'ON' if value_json.frost_enabled else 'OFF' }}", "");
        // FrostSense control switch (writes SetEnabled 5370:2 to the mower).
        pub_switch("frost_switch", "FrostSense",
            "{{ 'ON' if value_json.frost_enabled else 'OFF' }}",
            "frost_on", "frost_off");
        // Battery voltage (4106:1) and charging-wire loop signals (4462:13/14).
        pub_sensor("batt_voltage", "Battery voltage",
            "{{ value_json.batt_voltage }}", "V", "voltage");
        pub_sensor("loop_strength", "Loop signal strength",
            "{{ value_json.loop_strength }}", "%", "");
        pub_sensor("loop_a", "A-loop signal",
            "{{ value_json.loop_a }}", "", "");
        pub_sensor("loop_f", "F-loop signal",
            "{{ value_json.loop_f }}", "", "");
        pub_sensor("loop_guide", "Guide-wire signal",
            "{{ value_json.loop_guide }}", "", "");
        pub_dur("run_time",    "Total running time",   "run_time");
        pub_dur("cut_time",    "Total cutting time",   "cut_time");
        pub_dur("charge_time", "Total charging time",  "charge_time");
        pub_dur("search_time", "Total searching time", "search_time");
        pub_sensor("collisions", "Lifetime collisions",
            "{{ value_json.collisions }}", "", "");
        pub_sensor("charge_cycles", "Charging cycles",
            "{{ value_json.charge_cycles }}", "", "");
        pub_dur("blade_time", "Blade usage time", "blade_time");
        // Avoid garage (ChargingStation SetMowerHouseInstalled 4692:3) and
        // LawnSense (Autotimer): on/off + Low/Med/High sensitivity in one
        // Off/Low/Medium/High select.
        pub_switch("garage", "Avoid garage",
            "{{ 'ON' if value_json.garage else 'OFF' }}",
            "garage_on", "garage_off");
        pub_select4("lawnsense", "LawnSense",
            "Off", "Low", "Medium", "High",
            "{{ value_json.lawn_opt if value_json.lawn_opt is not none "
            "else 'Off' }}",
            "{{ {'Off':'lawn_off','Low':'lawn_low','Medium':'lawn_med',"
            "'High':'lawn_high'}[value] }}");
        pub_button("wake",  "Wake",  "wake");
        pub_button("mow",   "Mow",   "mow");
        pub_button("park",  "Park",  "park");
        pub_button("pause", "Pause", "pause");

        // Remove entities published by earlier firmware but since dropped as
        // unsupported on the GO 400 (empty retained payload = HA deletes it).
        unpub_cfg("sensor",        "board_temp");  // 20:4 mowertemp — unsupported
        unpub_cfg("binary_sensor", "upsidedown");  // 20:4 — unsupported
        unpub_cfg("sensor",        "theft");       // 4736:21 — unsupported
        debug_log::write(debug_log::INFO, SRC, "HA discovery published");
    }

    static bool reconnect() {
        String user = settings::get_mqtt_user();
        String pass = settings::get_mqtt_pass();
        // Client id derived from base; LWT marks us offline on ungraceful drop.
        bool ok = user.isEmpty()
            ? _client.connect(_base, _t_avail, 0, true, "offline")
            : _client.connect(_base, user.c_str(), pass.c_str(),
                              _t_avail, 0, true, "offline");
        if (ok) {
            _client.publish(_t_avail, "online", true);
            _client.subscribe(_t_cmd);
            pub_discovery();
            debug_log::write(debug_log::INFO, SRC,
                "connected to broker (sub %s)", _t_cmd);
        } else {
            debug_log::write(debug_log::WARN, SRC,
                "broker connect failed rc=%d", _client.state());
        }
        return ok;
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
        else                            strcpy(lo, "\"Unknown\"");
        if (st.stat_running    >= 0) snprintf(s_run, sizeof(s_run), "%ld", (long)st.stat_running);    else strcpy(s_run, "null");
        if (st.stat_cutting    >= 0) snprintf(s_cut, sizeof(s_cut), "%ld", (long)st.stat_cutting);    else strcpy(s_cut, "null");
        if (st.stat_charging   >= 0) snprintf(s_chg, sizeof(s_chg), "%ld", (long)st.stat_charging);   else strcpy(s_chg, "null");
        if (st.stat_searching  >= 0) snprintf(s_sea, sizeof(s_sea), "%ld", (long)st.stat_searching);  else strcpy(s_sea, "null");
        if (st.stat_collisions >= 0) snprintf(s_col, sizeof(s_col), "%ld", (long)st.stat_collisions); else strcpy(s_col, "null");
        if (st.stat_cycles     >= 0) snprintf(s_cyc, sizeof(s_cyc), "%ld", (long)st.stat_cycles);     else strcpy(s_cyc, "null");
        if (st.stat_blade      >= 0) snprintf(s_bld, sizeof(s_bld), "%ld", (long)st.stat_blade);      else strcpy(s_bld, "null");

        char buf[1024];
        snprintf(buf, sizeof(buf),
            "{\"conn\":\"%s\",\"state\":%s,\"activity\":%s,\"battery\":%s,"
            "\"charging\":%s,\"charge_left\":%s,\"error\":%s,"
            "\"restriction\":%s,\"next_start\":%s,"
            "\"batt_temp\":%s,\"pitch\":%s,\"roll\":%s,"
            "\"collision\":%s,\"lift\":%s,"
            "\"power_mode\":%s,\"frost_avail\":%s,\"frost_enabled\":%s,"
            "\"batt_voltage\":%s,\"loop_strength\":%s,"
            "\"loop_a\":%s,\"loop_f\":%s,\"loop_guide\":%s,"
            "\"garage\":%s,\"lawn_avail\":%s,\"lawn_opt\":%s,"
            "\"run_time\":%s,\"cut_time\":%s,\"charge_time\":%s,"
            "\"search_time\":%s,\"collisions\":%s,\"charge_cycles\":%s,"
            "\"blade_time\":%s}",
            conn_str(st.state), ms, ma, mb, mch, mcl, mer, mrs, mns,
            bt, pa, ra, co, li, pm, fa, fe,
            bv, ls, la, lf, lg,
            gg, lav, lo,
            s_run, s_cut, s_chg, s_sea, s_col, s_cyc, s_bld);
        _client.publish(_t_state, buf, true);
    }

    void loop() {
        if (!_configured) return;
        if (!wifi_manager::is_connected()) return;

        if (!_client.connected()) {
            uint32_t now = millis();
            if (now - _last_try < RECONNECT_MS) return;
            _last_try = now;
            if (!reconnect()) return;
        }

        _client.loop();

        uint32_t now = millis();
        if (now - _last_pub >= PUBLISH_MS) {
            _last_pub = now;
            publish_state();
        }
    }
}
