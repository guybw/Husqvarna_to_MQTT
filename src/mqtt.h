// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// MQTT bridge (M8). Publishes mower telemetry to:
//   flymo/<id>/state          JSON: conn, state, activity, battery
//   flymo/<id>/availability   "online" / "offline" (LWT)
// <id> = mower MAC with colons stripped.
//
// Broker config in NVS (settings::get_mqtt_*). Empty host = disabled.

namespace mqtt {
    void begin();
    void loop();
}
