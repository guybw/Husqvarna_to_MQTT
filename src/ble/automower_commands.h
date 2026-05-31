// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdint.h>

// High-level command encoders/decoders. Generated/translated from
// docs/upstream-protocol.json. Implemented in M5/M6.
//
// MVP command set (see plan):
//   EnterOperatorPin                      auth
//   GetState              (4586:2)        operation state
//   GetActivity           (4586:3)        what it's doing
//   GetMode               (4586:0)        auto/manual/home
//   GetMowerError         (see json)      latest error code
//   GetSerialNumber       (4698:10)       device serial -> MQTT topic + HA uid
//   GetBatteryLevel       (4106:20)       %
//   IsCharging            (4106:21)
//   GetRemainingChargingTime (4106:22)
//   GetNextStartTime      (4658:1)        unix ts
//   GetAllStatistics      (4726:0)        runtime / cycle counters
//   StartTrigger          (4586:4)        start / resume
//   Pause                 (4586:5)
//   SetOverridePark       (4658:4)
//   SetOverrideMow        (4658:3)        for N hours

namespace automower_cmd {
    bool enter_operator_pin(uint32_t pin);
    bool get_state(uint32_t* out_state);
    bool get_battery_level(uint8_t* out_pct);
    // TODO M6: full set.
}
