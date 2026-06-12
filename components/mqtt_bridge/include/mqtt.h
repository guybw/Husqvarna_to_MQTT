// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// MQTT bridge (M8). Publishes mower telemetry to:
//   flymo/<id>/state          JSON: conn, state, activity, battery
//   flymo/<id>/availability   "online" / "offline" (LWT)
// <id> = mower MAC with colons stripped.
//
// Broker config in NVS (settings::get_mqtt_*). Empty host = disabled.
//
// Native ESP-IDF port: uses esp-mqtt (esp_mqtt_client) instead of
// PubSubClient. esp-mqtt runs its own background task and auto-reconnects,
// so begin() inits+starts the client and loop() only throttles the periodic
// state publish. Connection lifecycle is handled in the event handler.

namespace mqtt {
    void begin();
    void loop();
}
