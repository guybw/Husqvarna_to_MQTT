// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

namespace ble_manager {

    static constexpr uint8_t  MAX_RESULTS      = 32;
    static constexpr uint32_t SCAN_DURATION_S  = 5;
    static constexpr uint32_t SCAN_INTERVAL_MS = 30000;

    struct Result {
        char   addr[18]; // "aa:bb:cc:dd:ee:ff"
        char   name[64];
        int8_t rssi;
        bool   mower;
    };

    enum class ConnState : uint8_t {
        IDLE = 0,       // Transient: a connection attempt is queued
        CONNECTING,     // NimBLE connect() in progress
        HANDSHAKING,    // Channel setup + PIN exchange
        AUTHENTICATED,  // Connected; polls every 8 s while the mower is active
                        // (mowing/stuck) and disconnects once it rests benignly
        ERROR,          // Last attempt failed (transient → DORMANT)
        DORMANT,        // Resting: disconnected, mower left to sleep. Re-checks
                        // on a long interval (or at the mower's next-start);
                        // force_wake() / a command connects immediately.
    };

    struct MowerStatus {
        ConnState state;
        char      addr[18];
        char      name[64];
        int8_t    rssi;
        char      detail[80]; // human-readable state detail
        int16_t   mower_state;    // MowerState enum; -1 = not yet polled
        int16_t   mower_activity; // MowerActivity enum; -1 = not yet polled
        int16_t   mower_battery;  // 0-100 %; -1 = not yet polled
        int8_t    mower_charging;     // 0/1; -1 = not yet polled
        int32_t   mower_charge_left; // remaining charge secs; -1 = unknown
        int32_t   mower_error;        // error code (0 = none); -1 = unknown
        int16_t   mower_restriction;  // restriction reason code; -1 = unknown
        int32_t   mower_next_start;   // next start unix ts (0 = none); -1 = unknown
        // Outdoor/diagnostic sensors (APK-recovered). Signed temps/angles use
        // INT16_MIN as the unknown sentinel; the 0/1 flags use -1.
        int16_t   mower_batt_temp;    // battery temp °C
        int16_t   mower_pitch;        // pitch angle °
        int16_t   mower_roll;         // roll angle °
        int8_t    mower_collision;    // 0/1
        int8_t    mower_lift;         // 0/1
        int8_t    mower_power_mode;   // uint8 power mode
        int8_t    mower_frost_avail;   // 0/1 FrostSense available
        int8_t    mower_frost_enabled; // 0/1 FrostSense enabled
        int32_t   mower_batt_mv;       // battery voltage mV; -1 = unknown
        int16_t   loop_strength;       // loop signal strength %; -1 = unknown
        int16_t   loop_a;              // A-loop signal (sint16)
        int16_t   loop_f;              // F-loop signal (sint16)
        int16_t   loop_guide;          // guide-wire (G1) signal (sint16)
        int8_t    mower_garage;        // 0/1 avoid-garage (mowerHouseInstalled)
        int8_t    lawn_avail;          // 0/1 LawnSense available
        int8_t    lawn_enabled;        // 0/1 LawnSense enabled
        int8_t    lawn_sens;           // LawnSense sensitivity 1=Low 2=Med 3=High
        int32_t   stat_running;       // total running time s
        int32_t   stat_cutting;       // total cutting time s
        int32_t   stat_charging;      // total charging time s
        int32_t   stat_searching;     // total searching time s
        int32_t   stat_collisions;    // lifetime collision count
        int32_t   stat_cycles;        // charging cycles
        int32_t   stat_blade;         // cutting-blade usage time s
        int16_t   drive_past;         // drive-past-wire distance cm (4712:0); -1
        int8_t    collision_resp;     // collision responsiveness 0=Low/1=Med/2=High; -1
    };

    void    begin();
    void    loop();
    void    trigger_scan();
    bool    is_scanning();
    uint8_t get_results(Result* out, uint8_t max_out);

    // Save MAC + PIN to NVS and kick off a connection attempt.
    void set_target(const char* mac, uint32_t pin);
    void disconnect_target();

    // Leave DORMANT and trigger a single connect+handshake attempt.
    void force_wake();

    // Idle re-check interval (minutes) — how often the bridge wakes a resting
    // mower. Persisted to NVS; takes effect immediately (clamped 1..1440).
    void     set_idle_recheck_minutes(uint32_t minutes);
    uint32_t get_idle_recheck_minutes();

    // Cease all BLE activity (drop link, stop scanning, idle the conn_task) so
    // an OTA flash gets the CPU/radio. One-way — the device reboots after OTA.
    void suspend();

    // Queue a command to send on the next AUTHENTICATED conn_task tick.
    // cmd: "mow" | "park" | "pause". If secs > 0, use that override
    // duration for mow or park.
    bool queue_command(const char* cmd, uint32_t secs = 0);

    struct ScheduleTask {
        uint32_t start;     // seconds since midnight
        uint32_t duration;  // seconds
        bool     use_on[7]; // Monday..Sunday
    };

    bool read_schedule(ScheduleTask* out, uint32_t max_out, uint32_t* out_count);
    bool write_schedule(const ScheduleTask* tasks, uint32_t count);
    // Like write_schedule but wakes the mower first if it is asleep (blocks the
    // caller up to ~45 s waiting for the link). For the web Save handler.
    bool write_schedule_wake(const ScheduleTask* tasks, uint32_t count);

    // Last schedule read from the mower while connected (cached; no BLE I/O).
    // Returns false if nothing has been cached yet. Safe to call from any task.
    bool get_schedule_cache(ScheduleTask* out, uint32_t max_out, uint32_t* out_count);

    MowerStatus get_mower_status();
}
