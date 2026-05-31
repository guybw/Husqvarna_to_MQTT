// SPDX-License-Identifier: GPL-3.0-or-later
#include "ble_manager.h"
#include "automower_protocol.h"
#include "crc8_maxim.h"
#include "../debug_log.h"
#include "../config.h"
#include "../settings.h"
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <atomic>
#include <string.h>

#define SRC "ble"

// Chunk size for BLE writes — mower accepts max 20-byte ATT payload;
// use 17 conservatively as documented in protocol-notes.md.
static constexpr size_t BLE_CHUNK = 17;

// Channel-setup link packet is 12 bytes.  Last two bytes are the "link
// request" type (0x02) and version (0x00) — empirically derived from
// Marbanz/HusqvarnaAutoMower-BLE.
static constexpr uint8_t LINK_TYPE_REQUEST = 0x02;
static constexpr uint8_t LINK_VERSION      = 0x00;

// Build the step-2 handshake packet (14 bytes).
// The outer ch_id is our established channel ID (NOT zero).
// Appends payload_crc = CRC8(bytes[1..11]) and 0x03 end marker.
static size_t build_handshake(uint8_t* out, uint32_t ch_id) {
    static const uint8_t TMPL[12] = {
        0x02, 0xFD,
        0x0A, 0x00,              // len=10, total=12
        0x00, 0x00, 0x00, 0x00, // outer ch_id (our channel id)
        0x00,                    // linked=0
        0x00,                    // hdr_crc (computed below)
        0x08,                    // msg_type
        0x01
    };
    memcpy(out, TMPL, 12);
    out[4] = (uint8_t)(ch_id & 0xFF);
    out[5] = (uint8_t)((ch_id >> 8) & 0xFF);
    out[6] = (uint8_t)((ch_id >> 16) & 0xFF);
    out[7] = (uint8_t)(ch_id >> 24);
    out[9]  = crc8::maxim(out + 1, 8);  // hdr_crc = CRC8(bytes[1..8])
    out[12] = crc8::maxim(out + 1, 11); // payload_crc = CRC8(bytes[1..11])
    out[13] = 0x03;
    return 14;
}

namespace ble_manager {

    // ── Scan state ────────────────────────────────────────────────────────
    static Result            _results[MAX_RESULTS];
    static uint8_t           _result_count = 0;
    static SemaphoreHandle_t _scan_mutex   = nullptr;
    static std::atomic<bool>     _scanning(false);
    static std::atomic<uint32_t> _last_scan_ms(0);

    // ── Connection state ──────────────────────────────────────────────────
    static std::atomic<uint8_t> _conn_state(static_cast<uint8_t>(ConnState::IDLE));
    static char                 _target_addr[18] = {};  // NVS-loaded MAC
    static uint32_t             _target_pin  = 1234;
    static char                 _mower_name[64] = {};
    static int8_t               _mower_rssi = 0;
    static char                 _state_detail[80] = "idle";

    // ── NimBLE client ─────────────────────────────────────────────────────
    static NimBLEClient*              _client      = nullptr;
    static NimBLERemoteCharacteristic* _write_char = nullptr;
    static NimBLERemoteCharacteristic* _notify_char = nullptr;

    // ── RX reassembly ─────────────────────────────────────────────────────
    static uint8_t          _rx_buf[512];
    static volatile size_t  _rx_len  = 0;
    static SemaphoreHandle_t _rx_mutex = nullptr;
    static SemaphoreHandle_t _rx_sem   = nullptr; // given by notify_cb

    // ── Polled mower telemetry ────────────────────────────────────────────
    static volatile int16_t  _mower_state    = -1; // -1 = not yet polled
    static volatile int16_t  _mower_activity = -1;
    static volatile int16_t  _mower_battery  = -1;
    static volatile int8_t   _mower_charging   = -1;
    static volatile int32_t  _mower_charge_left = -1;
    static volatile int32_t  _mower_error      = -1;
    static volatile int16_t  _mower_restriction = -1;
    static volatile int32_t  _mower_next_start = -1;
    // Outdoor/diagnostic sensors (recovered from Flymo APK, 2026-05-17 —
    // see docs/protocol-notes.md). Signed values use INT16_MIN as the
    // "not yet polled / unknown" sentinel because temperatures and tilt
    // angles can legitimately be negative.
    static constexpr int16_t UNK16 = INT16_MIN;
    static volatile int16_t  _mower_batt_temp  = UNK16; // °C  (4106:9)
    static volatile int16_t  _mower_pitch      = UNK16; // deg (4958:0)
    static volatile int16_t  _mower_roll       = UNK16; // deg (4958:1)
    static volatile int8_t   _mower_collision  = -1;    // 0/1 (4166:8)
    static volatile int8_t   _mower_lift       = -1;    // 0/1 (4476:6)
    static volatile int8_t   _mower_power_mode = -1;    // uint8 (4674:1)
    static volatile int8_t   _mower_frost_avail   = -1; // 0/1 (5370:8)
    static volatile int8_t   _mower_frost_enabled = -1; // 0/1 (5370:8)
    static volatile int32_t  _mower_batt_mv    = -1;    // mV  (4106:1, /1000 = V)
    static volatile int16_t  _loop_strength    = -1;    // %   (4462:14)
    static volatile int16_t  _loop_a           = UNK16; // sint16 (4462:13)
    static volatile int16_t  _loop_f           = UNK16; // sint16
    static volatile int16_t  _loop_guide       = UNK16; // sint16 (G1)
    static volatile int8_t   _mower_garage     = -1;    // 0/1 (4692:10 mowerHouseInstalled)
    static volatile int8_t   _lawn_avail       = -1;    // 0/1 (4460:8)
    static volatile int8_t   _lawn_enabled     = -1;    // 0/1 (4460:8)
    static volatile int8_t   _lawn_sens        = -1;    // uint8 (4460:8) 1=Low 2=Med 3=High
    static volatile int32_t  _stat_running     = -1;    // s   (4726:0)
    static volatile int32_t  _stat_cutting     = -1;    // s
    static volatile int32_t  _stat_charging    = -1;    // s
    static volatile int32_t  _stat_searching   = -1;    // s
    static volatile int32_t  _stat_collisions  = -1;
    static volatile int32_t  _stat_cycles      = -1;
    static volatile int32_t  _stat_blade       = -1;    // s
    // Stability: optional sensors are probed with a short timeout and, if the
    // mower doesn't answer (command unsupported on this model), disabled for
    // the rest of the BLE session so we never pay the timeout again. They are
    // also polled only every Nth tick — they change slowly and the core poll
    // (state/activity/battery/error) must stay fast or WiFi/MQTT/SSE starve.
    // batt_temp, pitch, roll, collision, lift, power, stats, frost,
    // batt_voltage, loop_strength, loop_signals, garage, lawnsense
    static bool _opt_supported[13];
    static uint8_t _poll_tick = 0;
    static constexpr uint32_t OPT_QUERY_TIMEOUT_MS = 1500;
    static constexpr uint8_t  OPT_POLL_EVERY       = 5; // ~ every 40 s
    static uint32_t          _last_poll_ms   = 0;

    // begin() sets this true. While false (maintenance mode skips begin()),
    // every public entry point no-ops so no BLE activity — including the
    // background scan — can start. NimBLE is never even initialised.
    static bool _started = false;

    // ── Poll-failure watchdog ─────────────────────────────────────────────
    // N consecutive failed polls = mower app layer went back to sleep →
    // disconnect and drop to DORMANT, then passively auto-retry on backoff.
    static uint8_t _poll_fail = 0;
    static constexpr uint8_t POLL_FAIL_LIMIT = 3;

    // ── Passive auto-reconnect ────────────────────────────────────────────
    // The mower's BLE app layer sleeps when docked/idle and is awake while
    // mowing. We want telemetry whenever it's awake, WITHOUT forcing a
    // sleeping mower awake. So the background retry connects passively (no
    // device-type wake-read): if the mower is mowing the handshake completes
    // and we stream; if it's asleep the handshake fails fast and we back off
    // and try again later. Only an explicit Wake / (re)pair does the
    // wake-read. Balanced backoff: 15 → 30 → 60 → 120 s, capped.
    static uint32_t _next_attempt_ms = 0;   // millis() of next allowed try
    static uint32_t _backoff_ms      = 0;   // 0 = first try is immediate
    static constexpr uint32_t BACKOFF_START_MS = 15000;
    static constexpr uint32_t BACKOFF_MAX_MS   = 120000;
    // When the poll-fail watchdog decides the mower slept, we want to leave
    // it alone for real (otherwise the reconnect+handshake itself re-wakes
    // its app layer every couple of minutes — see passive-but-not-really
    // observation 2026-05-19). This flag tells onDisconnect to apply the
    // long sleep backoff instead of the fast RF-glitch 10 s retry.
    static constexpr uint32_t SLEEP_BACKOFF_MS = 5UL * 60UL * 1000UL; // 5 min
    static std::atomic<bool>  _sleep_disconnect(false);
    // Set by force_wake()/set_target(): the NEXT attempt may do the
    // app-layer wake-read (explicit user intent). Auto attempts never do.
    static std::atomic<bool> _wake_read_pending(false);

    // ── Pending command (0=none, 1=mow, 2=park, 3=pause) ─────────────────
    static std::atomic<uint8_t> _pending_cmd(0);
    static std::atomic<uint32_t> _pending_cmd_secs(0);
    static SemaphoreHandle_t _pending_cmd_mutex = nullptr;
    static SemaphoreHandle_t _ble_io_mutex = nullptr;

    // ── Connection task ───────────────────────────────────────────────────
    static TaskHandle_t _conn_task_h = nullptr;

    // ═════════════════════════════════════════════════════════════════════
    // Helpers
    // ═════════════════════════════════════════════════════════════════════

    static void set_state(ConnState s, const char* detail) {
        _conn_state = static_cast<uint8_t>(s);
        strncpy(_state_detail, detail, sizeof(_state_detail) - 1);
        _state_detail[sizeof(_state_detail) - 1] = '\0';
        debug_log::write(debug_log::INFO, SRC, "conn state → %s (%s)",
            s == ConnState::IDLE         ? "IDLE"         :
            s == ConnState::CONNECTING   ? "CONNECTING"   :
            s == ConnState::HANDSHAKING  ? "HANDSHAKING"  :
            s == ConnState::AUTHENTICATED? "AUTHENTICATED":
            s == ConnState::DORMANT      ? "DORMANT"      :
                                          "ERROR",
            detail);
    }

    static bool write_frame(const uint8_t* data, size_t len) {
        if (!_write_char) return false;

        // Hex-dump the first 20 bytes for debugging.
        char hex[64] = {};
        size_t show = len < 20 ? len : 20;
        for (size_t i = 0; i < show; i++)
            snprintf(hex + i * 3, 4, "%02x ", data[i]);
        debug_log::write_serial(debug_log::DEBUG, SRC, "TX %u B: %s%s",
            (unsigned)len, hex, len > show ? "..." : "");

        size_t off = 0;
        while (off < len) {
            size_t chunk = (len - off < BLE_CHUNK) ? (len - off) : BLE_CHUNK;
            if (!_write_char->writeValue(data + off, chunk, false)) {
                debug_log::write(debug_log::ERROR, SRC,
                    "writeValue failed at offset %u", (unsigned)off);
                return false;
            }
            off += chunk;
        }
        return true;
    }

    // Clears the RX buffer and semaphore.
    static void rx_flush() {
        if (xSemaphoreTake(_rx_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            _rx_len = 0;
            xSemaphoreGive(_rx_mutex);
        }
        xSemaphoreTake(_rx_sem, 0); // drain any pending signal
    }

    // Wait for a complete frame (up to timeout_ms).
    static bool rx_wait(uint32_t timeout_ms) {
        return xSemaphoreTake(_rx_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    }

    // Copy the current RX frame into out_buf; returns length (0 = nothing ready).
    static size_t rx_take(uint8_t* out_buf, size_t out_max) {
        if (xSemaphoreTake(_rx_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return 0;
        size_t n = _rx_len < out_max ? _rx_len : out_max;
        memcpy(out_buf, _rx_buf, n);
        _rx_len = 0;
        xSemaphoreGive(_rx_mutex);
        return n;
    }

    // Build the 24-byte channel-setup link packet (Marbanz protocol.py format).
    //
    // The outer channel_id (bytes 4-7) is always 0 for this unlinked packet.
    // Our random channel_id goes into the inner payload at bytes 11-14.
    // Bytes 19-23 carry the client identity string "Main\0".
    // hdr_crc covers bytes 0-8 (outer header is fixed, crc is always 0x2e).
    static size_t build_channel_setup(uint8_t* out, uint32_t ch_id) {
        static const uint8_t TMPL[24] = {
            0x02, 0xFD,
            0x16, 0x00,              // len = 22 → total = 24
            0x00, 0x00, 0x00, 0x00, // outer ch_id = 0 (unlinked)
            0x00,                    // linked = 0
            0x00,                    // hdr_crc (computed below)
            0x14,                    // msg_type = link-request
            0x00, 0x00, 0x00, 0x00, // inner ch_id (our random value)
            0x00, 0x00, 0x00, 0x00, // reserved
            'M', 'a', 'i', 'n', 0x00 // client identity
        };
        memcpy(out, TMPL, 24);
        // Insert our channel ID at bytes 11-14 (inner payload position).
        out[11] = (uint8_t)(ch_id & 0xFF);
        out[12] = (uint8_t)((ch_id >> 8) & 0xFF);
        out[13] = (uint8_t)((ch_id >> 16) & 0xFF);
        out[14] = (uint8_t)(ch_id >> 24);
        out[9]  = crc8::maxim(out + 1, 8);  // hdr_crc = CRC8(bytes[1..8])
        out[24] = crc8::maxim(out + 1, 23); // payload_crc = CRC8(bytes[1..23])
        out[25] = 0x03;
        return 26;
    }

    // Send a no-payload query and wait for response. Returns payload byte count
    // in resp_out (up to resp_max bytes). Returns false on timeout/error.
    static bool _send_query(uint16_t major, uint16_t minor,
                            const uint8_t* payload, size_t payload_len,
                            uint8_t* resp_out, size_t resp_max, size_t* resp_len_out,
                            uint32_t timeout_ms = 5000) {
        if (!_ble_io_mutex || xSemaphoreTake(_ble_io_mutex, pdMS_TO_TICKS(5000)) != pdTRUE)
            return false;
        uint8_t buf[32];
        size_t n = automower::encode_request(buf, sizeof(buf), major, minor,
                                            payload, payload_len);
        if (n == 0) {
            xSemaphoreGive(_ble_io_mutex);
            return false;
        }
        rx_flush();
        bool ok = false;
        if (write_frame(buf, n) && rx_wait(timeout_ms)) {
            uint8_t raw[128]; size_t raw_n = rx_take(raw, sizeof(raw));
            uint16_t maj = 0, min = 0;
            const uint8_t* p = nullptr; size_t plen_resp = 0;
            if (automower::decode_response(raw, raw_n, &maj, &min, &p, &plen_resp)) {
                size_t copy = plen_resp < resp_max ? plen_resp : resp_max;
                if (resp_out && copy > 0) memcpy(resp_out, p, copy);
                if (resp_len_out) *resp_len_out = copy;
                ok = true;
            }
        }
        xSemaphoreGive(_ble_io_mutex);
        return ok;
    }

    static bool send_query(uint16_t major, uint16_t minor,
                           uint8_t* resp_out, size_t resp_max, size_t* resp_len_out,
                           uint32_t timeout_ms = 5000) {
        return _send_query(major, minor, nullptr, 0, resp_out, resp_max,
                           resp_len_out, timeout_ms);
    }

    // Send a command (optionally with payload) and wait for the mower's
    // response. Returns true if a well-formed response came back.
    static bool send_command(uint16_t major, uint16_t minor,
                             const uint8_t* payload, size_t plen) {
        if (!_ble_io_mutex || xSemaphoreTake(_ble_io_mutex, pdMS_TO_TICKS(5000)) != pdTRUE)
            return false;
        auto hex = [](const char* label, const uint8_t* b, size_t n) {
            char h[64] = {};
            size_t show = n < 18 ? n : 18;
            for (size_t i = 0; i < show; i++) snprintf(h + i*3, 4, "%02x ", b[i]);
            debug_log::write(debug_log::INFO, SRC, "%s %u B: %s%s",
                label, (unsigned)n, h, n > show ? "..." : "");
        };
        uint8_t buf[32];
        size_t n = automower::encode_request(buf, sizeof(buf),
                                             major, minor, payload, plen);
        if (n == 0) {
            debug_log::write(debug_log::WARN, SRC, "cmd: encode failed");
            xSemaphoreGive(_ble_io_mutex);
            return false;
        }
        bool ok = false;
        // Two attempts: the mower's app layer often needs a second try (same
        // as the handshake). Shorter timeout than send_query so a sleeping
        // mower can't block the conn task for long.
        for (int attempt = 1; attempt <= 2; attempt++) {
            if (!_client || !_client->isConnected()) {
                debug_log::write(debug_log::WARN, SRC, "cmd: link down");
                ok = false;
                break;
            }
            rx_flush();
            hex("cmd TX", buf, n);
            if (!write_frame(buf, n)) {
                debug_log::write(debug_log::WARN, SRC, "cmd: write failed");
                ok = false;
                break;
            }
            if (!rx_wait(2500)) {
                debug_log::write(debug_log::WARN, SRC,
                    "cmd: no response (attempt %d/2)", attempt);
                continue;
            }
            uint8_t raw[128]; size_t raw_n = rx_take(raw, sizeof(raw));
            hex("cmd RX", raw, raw_n);
            uint16_t maj = 0, min = 0;
            const uint8_t* rp = nullptr; size_t rplen = 0;
            if (automower::decode_response(raw, raw_n, &maj, &min, &rp, &rplen)) {
                ok = true;
                break;
            }
            debug_log::write(debug_log::WARN, SRC,
                "cmd: bad response %u B (attempt %d/2)",
                (unsigned)raw_n, attempt);
        }
        xSemaphoreGive(_ble_io_mutex);
        return ok;
    }

    static bool send_enter_pin(uint32_t pin) {
        uint8_t payload[2] = {
            (uint8_t)(pin & 0xFF),
            (uint8_t)((pin >> 8) & 0xFF)
        };
        uint8_t buf[22];
        size_t n = automower::encode_request(buf, sizeof(buf), 4664, 4, payload, 2);
        if (n == 0) return false;
        return write_frame(buf, n);
    }

    static uint32_t u32le(const uint8_t* b) {
        return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
               ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    }

    static bool decode_task(const uint8_t* resp, size_t len, ScheduleTask& out) {
        if (len < 17) return false;
        out.start = u32le(resp);
        out.duration = u32le(resp + 4);
        out.use_on[0] = resp[8] != 0;
        out.use_on[1] = resp[9] != 0;
        out.use_on[2] = resp[10] != 0;
        out.use_on[3] = resp[11] != 0;
        out.use_on[4] = resp[12] != 0;
        out.use_on[5] = resp[13] != 0;
        out.use_on[6] = resp[14] != 0;
        return true;
    }

    static bool get_number_of_tasks(uint32_t& out) {
        uint8_t val[4]; size_t vlen = 0;
        if (!_send_query(4690, 4, nullptr, 0, val, sizeof(val), &vlen) || vlen < 4)
            return false;
        out = u32le(val);
        return true;
    }

    static bool get_task(uint32_t task_id, ScheduleTask& out) {
        uint8_t req[4] = {
            (uint8_t)(task_id & 0xFF),
            (uint8_t)((task_id >> 8) & 0xFF),
            (uint8_t)((task_id >> 16) & 0xFF),
            (uint8_t)((task_id >> 24) & 0xFF)
        };
        uint8_t resp[32]; size_t len = 0;
        if (!_send_query(4690, 5, req, sizeof(req), resp, sizeof(resp), &len))
            return false;
        return decode_task(resp, len, out);
    }

    static bool start_task_transaction() {
        return send_command(4690, 10, nullptr, 0);
    }

    static bool delete_all_tasks() {
        return send_command(4690, 9, nullptr, 0);
    }

    static bool add_task(const ScheduleTask& task) {
        if (task.duration > 0xFFFF) return false;
        uint8_t payload[15] = {
            (uint8_t)(task.start & 0xFF),
            (uint8_t)((task.start >> 8) & 0xFF),
            (uint8_t)((task.start >> 16) & 0xFF),
            (uint8_t)((task.start >> 24) & 0xFF),
            (uint8_t)(task.duration & 0xFF),
            (uint8_t)((task.duration >> 8) & 0xFF),
            task.use_on[6] ? 1 : 0,
            0,
            task.use_on[0] ? 1 : 0,
            task.use_on[1] ? 1 : 0,
            task.use_on[2] ? 1 : 0,
            task.use_on[3] ? 1 : 0,
            task.use_on[4] ? 1 : 0,
            task.use_on[5] ? 1 : 0,
            0
        };
        return send_command(4690, 7, payload, sizeof(payload));
    }

    static bool commit_task_transaction() {
        return send_command(4690, 11, nullptr, 0);
    }

    static bool is_authenticated() {
        return static_cast<ConnState>(_conn_state.load()) == ConnState::AUTHENTICATED;
    }

    static bool _read_schedule(ScheduleTask* out, uint32_t max_out, uint32_t* out_count) {
        if (!out || !out_count) return false;
        if (!is_authenticated()) return false;
        uint32_t count = 0;
        if (!get_number_of_tasks(count)) return false;
        if (count > max_out) return false;
        for (uint32_t i = 0; i < count; i++) {
            if (!get_task(i, out[i])) return false;
        }
        *out_count = count;
        return true;
    }

    static bool _write_schedule(const ScheduleTask* tasks, uint32_t count) {
        if (!tasks || count > 16) return false;
        if (!is_authenticated()) return false;
        if (!start_task_transaction()) return false;
        if (!delete_all_tasks()) return false;
        for (uint32_t i = 0; i < count; i++) {
            if (!add_task(tasks[i])) return false;
        }
        return commit_task_transaction();
    }

    // Poll GetState, GetActivity, GetBatteryLevel and store in module variables.
    // Called once immediately after auth and then every BLE_POLL_INTERVAL_MS.
    // Keeps the mower session alive (prevents idle-timeout disconnect) and
    // provides fresh telemetry for the MQTT publisher.
    // Returns false if the mower did not answer GetState — i.e. its app layer
    // has gone back to sleep.  The caller treats repeated false as "mower
    // asleep" and drops to DORMANT instead of spamming failed queries.
    static bool do_status_poll() {
        uint8_t val[4]; size_t vlen = 0;
        if (send_query(4586, 2, val, sizeof(val), &vlen) && vlen >= 1) {
            _mower_state = (int16_t)val[0];
            debug_log::write(debug_log::INFO, SRC, "poll: state=%u", (unsigned)val[0]);
        } else {
            debug_log::write(debug_log::WARN, SRC, "poll: GetState failed");
            return false; // mower not answering — skip the rest, no point
        }
        if (!_client || !_client->isConnected()) return false;
        if (send_query(4586, 3, val, sizeof(val), &vlen) && vlen >= 1) {
            _mower_activity = (int16_t)val[0];
            debug_log::write(debug_log::INFO, SRC, "poll: activity=%u", (unsigned)val[0]);
        } else {
            debug_log::write(debug_log::WARN, SRC, "poll: GetActivity failed");
        }
        if (!_client || !_client->isConnected()) return false;
        if (send_query(4106, 20, val, sizeof(val), &vlen) && vlen >= 1) {
            _mower_battery = (int16_t)val[0];
            debug_log::write(debug_log::INFO, SRC, "poll: battery=%u%%", (unsigned)val[0]);
        } else {
            debug_log::write(debug_log::WARN, SRC, "poll: GetBatteryLevel failed");
        }

        // ── Extra best-effort sensors ──────────────────────────────────────
        // Failures here are non-fatal (don't trip the sleep watchdog); only
        // GetState failing means the mower slept.
        auto u32le = [](const uint8_t* b){
            return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                   ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
        };

        if (!_client || !_client->isConnected()) return true;
        if (send_query(4106, 21, val, sizeof(val), &vlen) && vlen >= 1)
            _mower_charging = val[0] ? 1 : 0;

        if (!_client || !_client->isConnected()) return true;
        if (send_query(4106, 22, val, sizeof(val), &vlen) && vlen >= 4)
            _mower_charge_left = (int32_t)u32le(val);

        if (!_client || !_client->isConnected()) return true;
        if (send_query(4586, 6, val, sizeof(val), &vlen) && vlen >= 4) {
            _mower_error = (int32_t)u32le(val);
            if (_mower_error != 0)
                debug_log::write(debug_log::WARN, SRC,
                    "poll: error code=%ld", (long)_mower_error);
        }

        if (!_client || !_client->isConnected()) return true;
        if (send_query(4658, 0, val, sizeof(val), &vlen) && vlen >= 1)
            _mower_restriction = (int16_t)val[0];

        if (!_client || !_client->isConnected()) return true;
        if (send_query(4658, 1, val, sizeof(val), &vlen) && vlen >= 4)
            _mower_next_start = (int32_t)u32le(val);

        // ── Outdoor/diagnostic sensors (APK-recovered, see protocol-notes) ──
        // Polled only every OPT_POLL_EVERY ticks (they change slowly) and with
        // a short timeout; a command that fails once is disabled for the rest
        // of the session so an unsupported command never costs a timeout on
        // every cycle (that starved WiFi/MQTT/SSE in 0.9.0-dev).
        if ((_poll_tick++ % OPT_POLL_EVERY) != 0) return true;

        auto s16le = [](const uint8_t* b){
            return (int16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
        };
        uint8_t big[32];

        // opt(i): returns true with vlen set only if the sensor is still
        // enabled and answered within the short timeout. On first failure it
        // logs once and disables index i until the next fresh session.
        auto opt = [&](uint8_t i, uint16_t maj, uint16_t min, size_t need) -> bool {
            if (!_opt_supported[i]) return false;
            if (!_client || !_client->isConnected()) return false;
            if (send_query(maj, min, big, sizeof(big), &vlen,
                           OPT_QUERY_TIMEOUT_MS) && vlen >= need)
                return true;
            _opt_supported[i] = false;
            debug_log::write(debug_log::WARN, SRC,
                "poll: %u:%u unsupported/slow — disabled for session",
                (unsigned)maj, (unsigned)min);
            return false;
        };

        // Command IDs are exactly what the official Flymo app calls (traced
        // through its use-cases). The 20:4 comboard bundle from 0.9.0 is gone:
        // the GO 400 doesn't answer it (board_temp/upsidedown were always
        // null). There is NO board/ambient temperature command anywhere in
        // the protocol — battery temp (4106:9) is the only thermal signal.
        if (opt(0, 4106, 9, 2))                       // battery temp, sint16 °C
            _mower_batt_temp = s16le(big);

        if (opt(1, 4958, 0, 2))                       // pitch angle, sint16
            _mower_pitch = s16le(big);

        if (opt(2, 4958, 1, 2))                       // roll angle, sint16
            _mower_roll = s16le(big);

        if (opt(3, 4166, 8, 2))                       // collision: front,rear bool
            _mower_collision = (big[0] || big[1]) ? 1 : 0;

        if (opt(4, 4476, 6, 1))                       // lift: front bool
            _mower_lift = big[0] ? 1 : 0;

        if (opt(5, 4674, 1, 1))                       // power mode, uint8
            _mower_power_mode = (int8_t)big[0];

        if (opt(6, 4726, 0, 28)) {                    // GetAllStatistics
            _stat_running    = (int32_t)u32le(big +  0);
            _stat_cutting    = (int32_t)u32le(big +  4);
            _stat_charging   = (int32_t)u32le(big +  8);
            _stat_searching  = (int32_t)u32le(big + 12);
            _stat_collisions = (int32_t)u32le(big + 16);
            _stat_cycles     = (int32_t)u32le(big + 20);
            _stat_blade      = (int32_t)u32le(big + 24);
        }

        if (opt(7, 5370, 8, 2)) {                     // FrostSense GetAllSettings
            _mower_frost_avail   = big[0] ? 1 : 0;
            _mower_frost_enabled = big[1] ? 1 : 0;
        }

        if (opt(8, 4106, 1, 2))                       // battery voltage uint16 mV
            _mower_batt_mv = (int32_t)((uint16_t)big[0] | ((uint16_t)big[1] << 8));

        if (opt(9, 4462, 14, 1))                      // loop signal strength %
            _loop_strength = (int16_t)big[0];

        if (opt(10, 4462, 13, 12)) {                  // GetLoopSignals 6×sint16
            _loop_a     = s16le(big + 0);             // A-loop
            _loop_f     = s16le(big + 2);             // F-loop
            _loop_guide = s16le(big + 6);             // G1 (guide wire)
        }

        if (opt(11, 4692, 10, 2))                     // ChargingStation settings
            _mower_garage = big[1] ? 1 : 0;           // [0]=eco [1]=mowerHouse

        if (opt(12, 4460, 8, 3)) {                    // Autotimer (LawnSense)
            _lawn_avail   = big[0] ? 1 : 0;
            _lawn_enabled = big[1] ? 1 : 0;
            _lawn_sens    = (int8_t)big[2];
        }

        return true;
    }

    // ═════════════════════════════════════════════════════════════════════
    // NimBLE callbacks
    // ═════════════════════════════════════════════════════════════════════

    static void notify_cb(NimBLERemoteCharacteristic* /*pChar*/,
                          uint8_t* pData, size_t length, bool /*isNotify*/) {
        if (!_rx_mutex || !_rx_sem) return;
        if (xSemaphoreTake(_rx_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;

        // Append into reassembly buffer.
        if (_rx_len + length > sizeof(_rx_buf)) {
            _rx_len = 0; // overflow — reset
        }
        memcpy(_rx_buf + _rx_len, pData, length);
        _rx_len += length;

        // Classify frame once we have ≥12 bytes (need byte[11] to distinguish types).
        // Standard Automower protocol frames have 0xAF at byte[11] and use len+4.
        // Handshake/link frames don't have 0xAF at byte[11] and use len+2.
        bool complete = false;
        if (_rx_len >= 12 && _rx_buf[0] == 0x02 && _rx_buf[1] == 0xFD) {
            uint16_t frame_len = (uint16_t)(_rx_buf[2] | ((uint16_t)_rx_buf[3] << 8));
            size_t expected = (_rx_buf[11] == 0xAF)
                ? (size_t)(frame_len + 4)   // standard frame: len = total - 4
                : (size_t)(frame_len + 2);  // handshake/link frame: len = total - 2
            if (_rx_len >= expected) {
                complete = true;
                _rx_len  = expected;
            }
        }

        // Log RX bytes for debugging.
        char hex[64] = {};
        size_t show = _rx_len < 20 ? _rx_len : 20;
        for (size_t i = 0; i < show; i++)
            snprintf(hex + i * 3, 4, "%02x ", _rx_buf[i]);
        debug_log::write_serial(debug_log::DEBUG, SRC, "RX %u B: %s%s",
            (unsigned)_rx_len, hex, _rx_len > show ? "..." : "");

        xSemaphoreGive(_rx_mutex);

        if (complete) xSemaphoreGive(_rx_sem);
    }

    class ClientCB : public NimBLEClientCallbacks {
        void onConnect(NimBLEClient* /*c*/) override {
            debug_log::write(debug_log::INFO, SRC, "BLE connected");
        }
        void onDisconnect(NimBLEClient* /*c*/) override {
            debug_log::write(debug_log::WARN, SRC, "BLE disconnected");
            _write_char  = nullptr;
            _notify_char = nullptr;
            // Intentionally DO NOT reset _mower_* telemetry on disconnect.
            // Sleep cycles are frequent (~5 min docked); blanking the HA
            // dashboard every cycle is worse than showing the last known
            // values. The "Bridge connection" entity reports DORMANT so
            // freshness is still visible. (UX choice 2026-05-20.)
            xSemaphoreGive(_rx_sem); // unblock any waiting rx_wait
            ConnState s = static_cast<ConnState>(_conn_state.load());
            if (s != ConnState::IDLE && s != ConnState::DORMANT) {
                if (_sleep_disconnect.exchange(false)) {
                    // Poll watchdog confirmed the mower slept — leave it
                    // alone for SLEEP_BACKOFF_MS, otherwise our reconnect
                    // would just wake it again.
                    _backoff_ms      = SLEEP_BACKOFF_MS;
                    _next_attempt_ms = (uint32_t)millis() + _backoff_ms;
                    set_state(ConnState::DORMANT, "mower slept — resting 5 min");
                } else {
                    // Unsolicited disconnect (RF glitch / session reset).
                    // Retry quickly and passively: if it's still mowing we
                    // reconnect within a tick; if it actually slept the next
                    // passive attempt will fail and the backoff grows.
                    _backoff_ms      = 0;
                    _next_attempt_ms = (uint32_t)millis();
                    set_state(ConnState::DORMANT, "disconnected — auto-retrying");
                }
            }
        }
    };
    static ClientCB _client_cb;

    // ═════════════════════════════════════════════════════════════════════
    // Scanner callbacks (unchanged from M4)
    // ═════════════════════════════════════════════════════════════════════

    static bool looks_like_mower(NimBLEAdvertisedDevice* dev) {
        String nlc(dev->getName().c_str()); nlc.toLowerCase();
        if (nlc.indexOf("husqvarna") >= 0) return true;
        if (nlc.indexOf("easilife")  >= 0) return true;
        if (nlc.indexOf("flymo")     >= 0) return true;
        if (nlc.indexOf("automower") >= 0) return true;
        if (nlc.indexOf("amtc")      >= 0) return true;
        if (dev->isAdvertisingService(NimBLEUUID(automower::SERVICE_UUID))) return true;
        return false;
    }

    class ScanCB : public NimBLEAdvertisedDeviceCallbacks {
        void onResult(NimBLEAdvertisedDevice* dev) override {
            String name(dev->getName().c_str());
            String addr(dev->getAddress().toString().c_str());
            int8_t rssi  = dev->getRSSI();
            bool   mower = looks_like_mower(dev);

            if (mower)
                debug_log::write(debug_log::INFO, SRC,
                    "MOWER: %s \"%s\" rssi=%d", addr.c_str(), name.c_str(), rssi);
            else
                debug_log::write(debug_log::TRACE, SRC,
                    "dev: %s \"%s\" rssi=%d", addr.c_str(), name.c_str(), rssi);

            if (!_scan_mutex) return;
            if (xSemaphoreTake(_scan_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
            if (_result_count < MAX_RESULTS) {
                Result& r = _results[_result_count++];
                strncpy(r.addr, addr.c_str(), sizeof(r.addr) - 1); r.addr[17] = 0;
                strncpy(r.name, name.c_str(), sizeof(r.name) - 1); r.name[63] = 0;
                r.rssi  = rssi;
                r.mower = mower;

                // Cache the mower's name+rssi for status reporting.
                if (mower) {
                    strncpy(_mower_name, name.c_str(), sizeof(_mower_name) - 1);
                    _mower_rssi = rssi;
                }
            }
            xSemaphoreGive(_scan_mutex);
        }
    };
    static ScanCB _scan_cb;

    static void on_scan_done(NimBLEScanResults results) {
        uint8_t n = 0;
        if (_scan_mutex && xSemaphoreTake(_scan_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            n = _result_count;
            xSemaphoreGive(_scan_mutex);
        }
        _scanning     = false;
        _last_scan_ms = (uint32_t)millis();
        debug_log::write(debug_log::INFO, SRC,
            "scan complete — %u device(s)", (unsigned)n);
    }

    // ═════════════════════════════════════════════════════════════════════
    // Connection / handshake sequence
    // ═════════════════════════════════════════════════════════════════════

    // do_wake_read: when true, read the device-type characteristic to force
    // the mower's GATT app layer awake (explicit Wake / pairing). When false
    // (background auto-retry) the read is skipped — a sleeping mower is left
    // asleep and the handshake simply fails fast.
    static bool do_connect_and_handshake(bool do_wake_read) {
        const char* addr = _target_addr;
        if (addr[0] == '\0') return false;

        // Stop scanning before connecting.
        if (_scanning) {
            NimBLEDevice::getScan()->stop();
            vTaskDelay(pdMS_TO_TICKS(200));
            _scanning = false;
        }

        set_state(ConnState::CONNECTING, "connecting");
        debug_log::write(debug_log::INFO, SRC, "connecting to %s", addr);

        // Always create a fresh client — reusing a disconnected client causes
        // stale state in the NimBLE stack and leads to subscribe failures.
        if (_client) {
            if (_client->isConnected()) _client->disconnect();
            NimBLEDevice::deleteClient(_client);
            _client = nullptr;
        }
        _client = NimBLEDevice::createClient();
        _client->setClientCallbacks(&_client_cb, false);

        // Try public address type first, then random.
        if (!_client->connect(NimBLEAddress(addr, BLE_ADDR_PUBLIC)) &&
            !_client->connect(NimBLEAddress(addr, BLE_ADDR_RANDOM))) {
            debug_log::write(debug_log::ERROR, SRC, "connect failed");
            return false;
        }

        // Secure the connection immediately after connecting, before any GATT
        // operations.  The mower opens a short pairing window after the user
        // enters the PIN physically; attempting security as early as possible
        // maximises the chance of catching that window.
        debug_log::write(debug_log::INFO, SRC, "securing connection...");
        if (_client->secureConnection()) {
            debug_log::write(debug_log::INFO, SRC, "connection secured");
        } else {
            debug_log::write(debug_log::WARN, SRC,
                "secureConnection() failed — continuing anyway");
        }

        // Discover service and characteristics.
        NimBLERemoteService* svc =
            _client->getService(NimBLEUUID(automower::SERVICE_UUID));
        if (!svc) {
            debug_log::write(debug_log::ERROR, SRC, "service not found");
            _client->disconnect();
            return false;
        }

        _write_char  = svc->getCharacteristic(NimBLEUUID(automower::CHAR_WRITE_UUID));
        _notify_char = svc->getCharacteristic(NimBLEUUID(automower::CHAR_NOTIFY_UUID));

        if (!_write_char || !_notify_char) {
            debug_log::write(debug_log::ERROR, SRC, "characteristic not found");
            _client->disconnect();
            return false;
        }

        // Read the device-type characteristic (98BD0004, read-only) before
        // subscribing.  The Flymo Android app and Marbanz reference both read
        // all readable characteristics during discovery.  This ATT_READ_REQ is
        // what wakes the mower's GATT application layer; without it the mower
        // stays asleep and returns an ATT error on the CCCD write (subscribe
        // no-response), preventing the handshake from ever progressing.
        // We then wait 2 s — the read wakes the app layer but its notification
        // machinery needs a moment to fully initialise before it can acknowledge
        // a CCCD write.
        if (do_wake_read) {
            NimBLERemoteCharacteristic* type_char =
                svc->getCharacteristic(NimBLEUUID(automower::CHAR_DEVICE_TYPE_UUID));
            if (type_char) {
                std::string val = type_char->readValue();
                debug_log::write(debug_log::INFO, SRC,
                    "device type read (%u bytes) — app layer woken", (unsigned)val.size());
            } else {
                debug_log::write(debug_log::WARN, SRC, "device type char not found");
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
        } else {
            // Passive (auto) attempt: do NOT wake a sleeping mower. If it's
            // mowing the app layer is already up and the handshake below
            // succeeds; if it's asleep phase-1 will time out and we back off.
            debug_log::write(debug_log::INFO, SRC,
                "passive connect — skipping wake-read (won't wake a sleeping mower)");
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        // Subscribe to notifications.
        // If the mower's BLE application stack is ready it acknowledges the CCCD
        // write (ATT Write Response) → subscribe(..., true) succeeds.
        // If it's not ready it returns an ATT error → subscribe(..., true) fails
        // and subscribe(..., false) silently sends the write without waiting for
        // the error.  In the no-response case notifications are NOT actually
        // enabled — phase 1 will always time out.  Fail fast here rather than
        // wasting 15 s on three phase-1 retries.
        // Subscribe to the notify characteristic.
        // With the app layer awake (device-type read above), the no-response
        // case may actually succeed — the mower processes the CCCD write but
        // doesn't ACK it.  Rather than bailing immediately, we continue into
        // phase 1 and let the channel-echo be the real readiness test.
        // If phase 1 times out we know the CCCD write wasn't actually processed
        // and we'll retry the whole connection with backoff as usual.
        bool sub_with_resp = _notify_char->subscribe(true, notify_cb, true);
        if (sub_with_resp) {
            debug_log::write(debug_log::INFO, SRC, "subscribed (with-response)");
        } else {
            debug_log::write(debug_log::WARN, SRC,
                "subscribe (with-response) failed");
            if (_notify_char->subscribe(true, notify_cb, false)) {
                debug_log::write(debug_log::WARN, SRC,
                    "subscribed (no-response) — proceeding to phase 1 to confirm");
            } else {
                debug_log::write(debug_log::ERROR, SRC, "subscribe failed (both modes)");
                _client->disconnect();
                return false;
            }
        }

        debug_log::write(debug_log::INFO, SRC,
            "GATT service+chars found, notifications subscribed");

        debug_log::write(debug_log::INFO, SRC, "MTU negotiated: %u",
            (unsigned)_client->getMTU());

        // Give the mower 5 s to settle AFTER subscription before sending any frames.
        // Matches Alistair23 reference: asyncio.sleep(5.0) comes after subscribe.
        vTaskDelay(pdMS_TO_TICKS(5000));

        set_state(ConnState::HANDSHAKING, "handshake");

        // ─────────────────────────────────────────────────────────────────
        // Handshake: log helper — shows bytes at INFO level so they appear
        // in the default (INFO) log filter on the Debug tab.
        // ─────────────────────────────────────────────────────────────────
        auto log_rx = [](const char* label, const uint8_t* buf, size_t n) {
            char hex[100] = {};
            size_t show = n < 20 ? n : 20;
            for (size_t i = 0; i < show; i++) snprintf(hex + i * 3, 4, "%02x ", buf[i]);
            debug_log::write(debug_log::INFO, SRC, "%s %u B: %s%s",
                label, (unsigned)n, hex, n > show ? "..." : "");
        };

        // ─────────────────────────────────────────────────────────────────
        // Phase 1: send channel-setup link packet with our channel ID.
        // The mower keeps its channel session open across BLE disconnects,
        // so we MUST reuse the same channel ID on reconnect — if we send a
        // different ID the mower treats it as an unknown new client and
        // rejects the CCCD write (subscribed no-response).
        // We persist the ID in NVS so it survives ESP32 reboots.
        // ─────────────────────────────────────────────────────────────────
        uint32_t ch_id = settings::get_channel_id();
        if (ch_id == 0) {
            ch_id = esp_random();
            if (ch_id == 0) ch_id = 1; // 0 is our sentinel for "not set"
            settings::set_channel_id(ch_id);
            debug_log::write(debug_log::INFO, SRC,
                "HS phase1: new channel ch_id=0x%08x (saved)", (unsigned)ch_id);
        } else {
            debug_log::write(debug_log::INFO, SRC,
                "HS phase1: reusing channel ch_id=0x%08x", (unsigned)ch_id);
        }
        automower::set_channel_id(ch_id);

        uint8_t ch_pkt[26];
        build_channel_setup(ch_pkt, ch_id);

        // Wait for mower's channel echo (it mirrors our channel ID back).
        // Retry up to 3 times; onDisconnect() gives rx_sem to unblock any
        // waiting rx_wait, so check isConnected() + byte count after waking.
        bool phase1_ok = false;
        for (int attempt = 0; attempt < 3 && !phase1_ok; attempt++) {
            if (attempt > 0) {
                if (!_client || !_client->isConnected()) break;
                debug_log::write(debug_log::INFO, SRC,
                    "HS phase1: retry %d (waiting 2s)", attempt + 1);
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
            rx_flush();
            if (!write_frame(ch_pkt, sizeof(ch_pkt))) {
                debug_log::write(debug_log::ERROR, SRC, "HS phase1: write failed");
                _client->disconnect();
                return false;
            }
            if (rx_wait(5000)) {
                if (!_client || !_client->isConnected()) {
                    debug_log::write(debug_log::WARN, SRC,
                        "HS phase1: disconnected during wait");
                    return false; // onDisconnect already handled backoff
                }
                uint8_t buf[128]; size_t n = rx_take(buf, sizeof(buf));
                if (n > 0) {
                    log_rx("HS ch echo", buf, n);
                    phase1_ok = true;
                } else {
                    debug_log::write(debug_log::WARN, SRC,
                        "HS phase1: spurious wake, no data (attempt %d/3)", attempt + 1);
                }
            } else {
                debug_log::write(debug_log::WARN, SRC,
                    "HS phase1: no echo (attempt %d/3)", attempt + 1);
            }
        }
        if (!phase1_ok) {
            if (_client && _client->isConnected()) {
                debug_log::write(debug_log::WARN, SRC,
                    "HS phase1: all attempts failed — mower not ready, retrying");
                _client->disconnect();
            }
            return false;
        }

        // ─────────────────────────────────────────────────────────────────
        // Phase 2: send fixed handshake-confirmation bytes.
        // ─────────────────────────────────────────────────────────────────
        debug_log::write(debug_log::INFO, SRC, "HS phase2: sending handshake");
        uint8_t hs_pkt[14];
        build_handshake(hs_pkt, ch_id);
        rx_flush();
        if (!write_frame(hs_pkt, sizeof(hs_pkt))) {
            debug_log::write(debug_log::ERROR, SRC, "HS phase2: write failed");
            _client->disconnect();
            return false;
        }

        if (rx_wait(3000)) {
            uint8_t buf[128]; size_t n = rx_take(buf, sizeof(buf));
            log_rx("HS phase2 resp", buf, n);
        } else {
            debug_log::write(debug_log::INFO, SRC, "HS phase2: no response (ok)");
        }

        // ─────────────────────────────────────────────────────────────────
        // Phase 3: EnterOperatorPin
        // ─────────────────────────────────────────────────────────────────
        set_state(ConnState::HANDSHAKING, "EnterOperatorPin");
        debug_log::write(debug_log::INFO, SRC,
            "HS phase3: PIN=%u", (unsigned)_target_pin);

        rx_flush();
        if (!send_enter_pin(_target_pin)) {
            debug_log::write(debug_log::ERROR, SRC, "HS phase3: write failed");
            _client->disconnect();
            return false;
        }

        if (!rx_wait(BLE_RESPONSE_TIMEOUT_MS)) {
            debug_log::write(debug_log::ERROR, SRC, "HS phase3: PIN response timeout");
            _client->disconnect();
            return false;
        }

        // Guard against the disconnect-unblocking-rx_sem race: if onDisconnect
        // gave the semaphore to unblock us, the connection is dead — don't declare
        // AUTHENTICATED on a corpse.
        if (!_client || !_client->isConnected()) {
            debug_log::write(debug_log::ERROR, SRC,
                "HS phase3: disconnected during PIN wait — retrying");
            return false;
        }

        {
            uint8_t resp[128]; size_t n = rx_take(resp, sizeof(resp));
            log_rx("HS PIN resp", resp, n);
            uint16_t maj = 0, min = 0;
            const uint8_t* payload = nullptr; size_t plen = 0;
            if (n > 0 && automower::decode_response(resp, n, &maj, &min, &payload, &plen)) {
                debug_log::write(debug_log::INFO, SRC,
                    "HS PIN decoded major=%u minor=%u plen=%u",
                    (unsigned)maj, (unsigned)min, (unsigned)plen);
            } else {
                debug_log::write(debug_log::WARN, SRC,
                    "HS PIN resp not a standard frame — assuming OK");
            }
        }

        // ─────────────────────────────────────────────────────────────────
        // Authenticated — do the initial status poll immediately. The 8 s poll
        // loop (in conn_task_fn) is what keeps the mower session alive; polling
        // now prevents the mower closing the session before the first tick.
        // ─────────────────────────────────────────────────────────────────
        _poll_fail = 0; // fresh session
        for (auto& s : _opt_supported) s = true; // re-probe optional sensors
        _poll_tick = 0;
        set_state(ConnState::AUTHENTICATED, "authenticated");

        _last_poll_ms = (uint32_t)millis();
        do_status_poll();

        return true;
    }

    // ═════════════════════════════════════════════════════════════════════
    // Connection task — runs in its own FreeRTOS task so that
    // NimBLEClient::connect() (which blocks) doesn't freeze Arduino loop().
    // ═════════════════════════════════════════════════════════════════════

    static void conn_task_fn(void*) {
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(500));

            ConnState s = static_cast<ConnState>(_conn_state.load());
            uint32_t  now = (uint32_t)millis();

            // Grow the backoff (15 → 30 → 60 → 120 s, capped) and arm the
            // next-attempt timer. Called after every failed attempt.
            auto back_off = [&](const char* why) {
                _backoff_ms = (_backoff_ms == 0) ? BACKOFF_START_MS
                            : (_backoff_ms >= BACKOFF_MAX_MS ? BACKOFF_MAX_MS
                                                             : _backoff_ms * 2);
                _next_attempt_ms = (uint32_t)millis() + _backoff_ms;
                set_state(ConnState::DORMANT, why);
            };

            // ERROR: a failed attempt — schedule a backed-off retry.
            if (s == ConnState::ERROR) {
                back_off("asleep — auto-retrying");
                continue;
            }

            // DORMANT: passive background auto-retry. Never force-wakes a
            // sleeping mower (wake-read skipped) — it just catches the mower
            // whenever it's awake (mowing). Honours the backoff window.
            if (s == ConnState::DORMANT) {
                if (_target_addr[0] == '\0') continue;            // no target
                if (_scanning) continue;                           // scanning
                if ((int32_t)(now - _next_attempt_ms) < 0) continue; // too soon

                bool wake = _wake_read_pending.exchange(false);
                if (do_connect_and_handshake(wake)) {
                    _backoff_ms = 0;                  // connected: reset
                } else {
                    back_off("asleep — auto-retrying");
                }
                continue;
            }

            // IDLE: explicit immediate attempt (Wake button / new target /
            // pairing). May do the wake-read to force the app layer awake.
            if (s == ConnState::IDLE) {
                if (_target_addr[0] == '\0') continue;
                if (_scanning) continue;

                bool wake = _wake_read_pending.exchange(false);
                if (do_connect_and_handshake(wake)) {
                    _backoff_ms = 0;
                } else {
                    back_off("connect failed — auto-retrying");
                }
                continue;
            }

            // AUTHENTICATED: poll status every 8s (the poll itself keeps the
            // mower session alive), dispatch any queued command.
            if (s == ConnState::AUTHENTICATED) {
                uint8_t cmd = 0;
            uint32_t cmd_secs = 0;
            if (_pending_cmd_mutex && xSemaphoreTake(_pending_cmd_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                cmd = _pending_cmd;
                if (cmd != 0) {
                    _pending_cmd = 0;
                    cmd_secs = _pending_cmd_secs;
                    _pending_cmd_secs = 0;
                }
                xSemaphoreGive(_pending_cmd_mutex);
            }
            if (cmd != 0) {
                uint16_t major = 0, minor = 0;
                const char* cmd_name = "";
                uint8_t  payload[4]; size_t plen = 0;
                if (cmd == 1) {
                        // Mow = SetOverrideMow (4658/3) with a uint32 duration
                        // in seconds, then a StartTrigger (4586/4) to leave pause.
                        // SetOverrideMow forces mowing even when the mower is
                        // RESTRICTED (outside its schedule); the follow-up trigger
                        // is required to resume a paused mower.
                        uint32_t secs = cmd_secs ? cmd_secs : settings::get_mow_override_secs();
                        major = 4658; minor = 3; cmd_name = "mow";
                        payload[0] = (uint8_t)(secs & 0xFF);
                        payload[1] = (uint8_t)((secs >> 8) & 0xFF);
                        payload[2] = (uint8_t)((secs >> 16) & 0xFF);
                        payload[3] = (uint8_t)((secs >> 24) & 0xFF);
                        plen = 4;
                        debug_log::write(debug_log::INFO, SRC,
                            "cmd: mow override %lus", (unsigned long)secs);
                    }
                    else if (cmd == 2) {
                        cmd_name = "park";
                        if (cmd_secs) {
                            major = 4658; minor = 4;
                            payload[0] = (uint8_t)(cmd_secs & 0xFF);
                            payload[1] = (uint8_t)((cmd_secs >> 8) & 0xFF);
                            payload[2] = (uint8_t)((cmd_secs >> 16) & 0xFF);
                            payload[3] = (uint8_t)((cmd_secs >> 24) & 0xFF);
                            plen = 4;
                            debug_log::write(debug_log::INFO, SRC,
                                "cmd: park override %lus", (unsigned long)cmd_secs);
                        } else {
                            major = 4658; minor = 5;
                        }
                    }
                    else if (cmd == 3) { major = 4586; minor = 5; cmd_name = "pause"; }
                    else if (cmd == 4 || cmd == 5) {
                        // FrostSense SetEnabled (5370:2, bool payload). Module
                        // 5370 confirmed for the GO 400 from GetAllSettings.
                        major = 5370; minor = 2;
                        cmd_name = (cmd == 4) ? "frost_on" : "frost_off";
                        payload[0] = (cmd == 4) ? 1 : 0;
                        plen = 1;
                    }
                    else if (cmd == 6 || cmd == 7) {
                        // Avoid garage = ChargingStation SetMowerHouseInstalled
                        major = 4692; minor = 3;
                        cmd_name = (cmd == 6) ? "garage_on" : "garage_off";
                        payload[0] = (cmd == 6) ? 1 : 0;
                        plen = 1;
                    }
                    else if (cmd == 8) {
                        // LawnSense Off = Autotimer SetEnabled(false) (4460:5)
                        major = 4460; minor = 5; cmd_name = "lawn_off";
                        payload[0] = 0; plen = 1;
                    }
                    else if (cmd >= 9 && cmd <= 11) {
                        // LawnSense Low/Med/High = Autotimer SetEnabled(true)
                        // (4460:5) + SetSensitivity 1/2/3 (4460:7). Two writes;
                        // leave major=0 so the generic single-send is skipped.
                        uint8_t sens = (uint8_t)(cmd - 8); // 9→1,10→2,11→3
                        uint8_t en = 1;
                        const char* nm = (cmd == 9) ? "lawn_low"
                                       : (cmd == 10) ? "lawn_med" : "lawn_high";
                        debug_log::write(debug_log::INFO, SRC,
                            "cmd: sending %s", nm);
                        bool ok1 = send_command(4460, 5, &en, 1);
                        bool ok2 = send_command(4460, 7, &sens, 1);
                        debug_log::write(ok1 && ok2 ? debug_log::INFO
                                                    : debug_log::WARN, SRC,
                            "cmd: %s %s", nm, (ok1 && ok2) ? "OK" : "failed");
                    }
                    if (major != 0) {
                        debug_log::write(debug_log::INFO, SRC, "cmd: sending %s", cmd_name);
                        bool ok = false;
                        if (cmd == 1 || cmd == 2) {
                            // For mow and park: send the override, then a StartTrigger
                            // so a paused mower acts on the override (resume then
                            // immediately perform the override action).
                            ok = send_command(major, minor,
                                              plen ? payload : nullptr, plen);
                            if (ok) {
                                debug_log::write(debug_log::INFO, SRC,
                                                 "cmd: start trigger");
                                ok = send_command(4586, 4, nullptr, 0);
                                debug_log::write(ok ? debug_log::INFO : debug_log::WARN, SRC,
                                                 "cmd: start trigger %s", ok ? "OK" : "failed");
                            }
                        } else {
                            ok = send_command(major, minor,
                                              plen ? payload : nullptr, plen);
                        }
                        debug_log::write(ok ? debug_log::INFO : debug_log::WARN, SRC,
                                         "cmd: %s %s", cmd_name, ok ? "OK" : "failed");
                    }
                }
                uint32_t now = (uint32_t)millis();
                if (now - _last_poll_ms >= (uint32_t)BLE_POLL_INTERVAL_MS) {
                    _last_poll_ms = now;
                    if (do_status_poll()) {
                        _poll_fail = 0;
                    } else if (++_poll_fail >= POLL_FAIL_LIMIT) {
                        // Mower stopped answering = it finished and slept.
                        // Mark this disconnect as a sleep so onDisconnect
                        // applies the long sleep backoff (5 min) instead of
                        // the fast 10 s RF-glitch retry — otherwise the
                        // reconnect+handshake itself re-wakes its app layer
                        // every ~2 min, defeating the whole point.
                        debug_log::write(debug_log::INFO, SRC,
                            "mower stopped answering — slept; backing off 5 min");
                        _sleep_disconnect = true;
                        if (_client) _client->disconnect();
                        // (onDisconnect sets DORMANT + the 5 min backoff.)
                    }
                }
            }
        }
    }

    // ═════════════════════════════════════════════════════════════════════
    // Public API
    // ═════════════════════════════════════════════════════════════════════

    void begin() {
        _scan_mutex = xSemaphoreCreateMutex();
        _rx_mutex   = xSemaphoreCreateMutex();
        _rx_sem     = xSemaphoreCreateBinary();
        _pending_cmd_mutex = xSemaphoreCreateMutex();
        _ble_io_mutex = xSemaphoreCreateMutex();

        NimBLEDevice::init("");
        NimBLEDevice::setMTU(517); // request larger MTU to match Android app behaviour

        // Security: "Just Works" pairing (no IO capability, no MITM).
        // The mower's CCCD appears to require an encrypted link for write
        // operations while allowing unauthenticated reads — so we must
        // establish security before subscribing.  No bonding so stale keys
        // don't cause issues across mower reboots.
        NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
        NimBLEDevice::setSecurityAuth(true, false, true); // bond=YES, MITM=no, SC=yes

        NimBLEScan* scan = NimBLEDevice::getScan();
        scan->setAdvertisedDeviceCallbacks(&_scan_cb, false);
        scan->setActiveScan(true);
        scan->setInterval(100);
        scan->setWindow(99);

        // Load stored target from NVS.
        String mac = settings::get_mower_mac();
        if (!mac.isEmpty()) {
            strncpy(_target_addr, mac.c_str(), sizeof(_target_addr) - 1);
            _target_pin = settings::get_mower_pin();
            uint32_t saved_ch = settings::get_channel_id();
            debug_log::write(debug_log::INFO, SRC,
                "target loaded from NVS: %s pin=%u ch_id=0x%08x",
                _target_addr, (unsigned)_target_pin, (unsigned)saved_ch);
            // DORMANT now means "passively auto-retry": from boot we attempt
            // a no-wake connect (won't wake a sleeping mower) and keep
            // retrying on backoff, so a headless unit captures every mow
            // without anyone pressing Wake. _next_attempt_ms = 0 → first try
            // is immediate; _wake_read_pending stays false → passive.
            set_state(ConnState::DORMANT, "boot — auto-retrying (passive)");
        }

        // Start connection task (stack 6144 bytes, priority 1).
        xTaskCreate(conn_task_fn, "ble_conn", 6144, nullptr, 1, &_conn_task_h);

        _started = true;
        debug_log::write(debug_log::INFO, SRC,
            "NimBLE ready (passive auto-reconnect; Wake = force-wake for cmds)");
    }

    void trigger_scan() {
        if (!_started) return;   // maintenance mode: begin() was skipped
        if (_scanning) return;
        ConnState s = static_cast<ConnState>(_conn_state.load());
        if (s == ConnState::CONNECTING || s == ConnState::HANDSHAKING ||
            s == ConnState::AUTHENTICATED) {
            debug_log::write(debug_log::DEBUG, SRC,
                "scan skipped — BLE connection active");
            return;
        }
        if (_scan_mutex && xSemaphoreTake(_scan_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            _result_count = 0;
            xSemaphoreGive(_scan_mutex);
        }
        _scanning = true;
        NimBLEDevice::getScan()->start(SCAN_DURATION_S, on_scan_done, true);
        debug_log::write(debug_log::INFO, SRC, "BLE scan started (%us)", SCAN_DURATION_S);
    }

    bool is_scanning() { return _scanning; }

    uint8_t get_results(Result* out, uint8_t max_out) {
        if (!_scan_mutex || !out) return 0;
        if (xSemaphoreTake(_scan_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return 0;
        uint8_t n = (_result_count < max_out) ? _result_count : max_out;
        memcpy(out, _results, n * sizeof(Result));
        xSemaphoreGive(_scan_mutex);
        return n;
    }

    void set_target(const char* mac, uint32_t pin) {
        if (!_started) return;   // maintenance mode: BLE off
        // Only reset the channel ID if the MAC is changing — a same-mower re-pair
        // should reuse the existing channel (mower keeps session open across disconnects).
        bool mac_changed = (settings::get_mower_mac() != String(mac));
        settings::set_mower_mac(String(mac));
        settings::set_mower_pin(pin);
        if (mac_changed) {
            settings::set_channel_id(0);
            debug_log::write(debug_log::INFO, SRC, "new mower MAC — channel ID reset");
        }
        strncpy(_target_addr, mac, sizeof(_target_addr) - 1);
        _target_addr[sizeof(_target_addr) - 1] = '\0';
        _target_pin = pin;

        ConnState s = static_cast<ConnState>(_conn_state.load());

        // If already connecting/handshaking with the SAME mac, just update the
        // pin and leave the in-progress attempt running — don't restart it.
        if ((s == ConnState::CONNECTING || s == ConnState::HANDSHAKING) &&
            strncmp(_target_addr, mac, sizeof(_target_addr)) == 0) {
            debug_log::write(debug_log::INFO, SRC,
                "target pin updated to %u (connection already in progress)", (unsigned)pin);
            return;
        }

        // For any other active state, disconnect cleanly before restarting.
        if (s != ConnState::IDLE && s != ConnState::ERROR &&
            s != ConnState::DORMANT) {
            if (_client && _client->isConnected()) _client->disconnect();
        }

        // (Re)pairing is an explicit user action — the next attempt may do
        // the app-layer wake-read, and it should happen immediately.
        _wake_read_pending = true;
        _backoff_ms        = 0;
        _next_attempt_ms   = (uint32_t)millis();
        // conn_task_fn will pick up the new target on the next loop tick.
        set_state(ConnState::IDLE, "new target set");
        debug_log::write(debug_log::INFO, SRC,
            "target set: %s pin=%u", mac, (unsigned)pin);
    }

    void disconnect_target() {
        if (!_started) return;   // maintenance mode: BLE off
        if (_client) _client->disconnect(); // also aborts in-progress connect()
        _target_addr[0] = '\0';
        settings::set_mower_mac(String(""));
        set_state(ConnState::IDLE, "disconnected by user");
        debug_log::write(debug_log::INFO, SRC, "disconnected by user");
    }

    void force_wake() {
        if (!_started) return;   // maintenance mode: BLE off
        if (_target_addr[0] == '\0') {
            debug_log::write(debug_log::WARN, SRC,
                "wake requested but no target configured");
            return;
        }
        ConnState s = static_cast<ConnState>(_conn_state.load());

        // Already streaming — nothing to wake.
        if (s == ConnState::AUTHENTICATED) {
            debug_log::write(debug_log::INFO, SRC,
                "wake ignored — already authenticated");
            return;
        }

        // Always arm the wake-read + immediate retry. If a passive auto-
        // attempt is currently in flight (CONNECTING/HANDSHAKING) we ABORT
        // it so the next task tick re-attempts WITH the wake-read — without
        // this preemption, Wake was a no-op whenever an auto-retry happened
        // to be running, which made the Web UI button feel broken.
        _poll_fail         = 0;
        _wake_read_pending = true;
        _backoff_ms        = 0;
        _next_attempt_ms   = (uint32_t)millis();

        if (s == ConnState::CONNECTING || s == ConnState::HANDSHAKING) {
            debug_log::write(debug_log::INFO, SRC,
                "wake requested — aborting in-flight passive attempt");
            if (_client && _client->isConnected()) _client->disconnect();
            // onDisconnect → DORMANT; next tick re-attempts with wake-read.
            return;
        }

        // DORMANT / ERROR / IDLE → kick an immediate IDLE attempt.
        set_state(ConnState::IDLE, "wake requested — connecting");
        debug_log::write(debug_log::INFO, SRC,
            "wake requested via API — attempting connection (wake-read)");
    }

    bool queue_command(const char* cmd, uint32_t secs) {
        if (!_started) return false;   // maintenance mode: BLE off
        uint8_t c = 0;
        if      (strcmp(cmd, "mow")       == 0) c = 1;
        else if (strcmp(cmd, "park")      == 0) c = 2;
        else if (strcmp(cmd, "pause")     == 0) c = 3;
        else if (strcmp(cmd, "frost_on")  == 0) c = 4;
        else if (strcmp(cmd, "frost_off") == 0) c = 5;
        else if (strcmp(cmd, "garage_on") == 0) c = 6;
        else if (strcmp(cmd, "garage_off")== 0) c = 7;
        else if (strcmp(cmd, "lawn_off")  == 0) c = 8;
        else if (strcmp(cmd, "lawn_low")  == 0) c = 9;
        else if (strcmp(cmd, "lawn_med")  == 0) c = 10;
        else if (strcmp(cmd, "lawn_high") == 0) c = 11;
        else return false;
        if (_pending_cmd_mutex && xSemaphoreTake(_pending_cmd_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            _pending_cmd_secs = secs;
            _pending_cmd = c;
            xSemaphoreGive(_pending_cmd_mutex);
        } else {
            _pending_cmd.store(c);
            _pending_cmd_secs = secs;
        }
        debug_log::write(debug_log::INFO, SRC, "command queued: %s", cmd);
        return true;
    }

    bool read_schedule(ScheduleTask* out, uint32_t max_out, uint32_t* out_count) {
        return _read_schedule(out, max_out, out_count);
    }

    bool write_schedule(const ScheduleTask* tasks, uint32_t count) {
        return _write_schedule(tasks, count);
    }

    MowerStatus get_mower_status() {
        MowerStatus st;
        st.state = static_cast<ConnState>(_conn_state.load());
        strncpy(st.addr, _target_addr, sizeof(st.addr));
        if (_scan_mutex && xSemaphoreTake(_scan_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            strncpy(st.name, _mower_name, sizeof(st.name));
            st.rssi = _mower_rssi;
            xSemaphoreGive(_scan_mutex);
        } else {
            st.name[0] = '\0';
            st.rssi = 0;
        }
        strncpy(st.detail, _state_detail, sizeof(st.detail));
        st.mower_state    = _mower_state;
        st.mower_activity = _mower_activity;
        st.mower_battery  = _mower_battery;
        st.mower_charging    = _mower_charging;
        st.mower_charge_left = _mower_charge_left;
        st.mower_error       = _mower_error;
        st.mower_restriction = _mower_restriction;
        st.mower_next_start  = _mower_next_start;
        st.mower_batt_temp   = _mower_batt_temp;
        st.mower_pitch       = _mower_pitch;
        st.mower_roll        = _mower_roll;
        st.mower_collision   = _mower_collision;
        st.mower_lift        = _mower_lift;
        st.mower_power_mode  = _mower_power_mode;
        st.mower_frost_avail   = _mower_frost_avail;
        st.mower_frost_enabled = _mower_frost_enabled;
        st.mower_batt_mv       = _mower_batt_mv;
        st.loop_strength       = _loop_strength;
        st.loop_a              = _loop_a;
        st.loop_f              = _loop_f;
        st.loop_guide          = _loop_guide;
        st.mower_garage        = _mower_garage;
        st.lawn_avail          = _lawn_avail;
        st.lawn_enabled        = _lawn_enabled;
        st.lawn_sens           = _lawn_sens;
        st.stat_running      = _stat_running;
        st.stat_cutting      = _stat_cutting;
        st.stat_charging     = _stat_charging;
        st.stat_searching    = _stat_searching;
        st.stat_collisions   = _stat_collisions;
        st.stat_cycles       = _stat_cycles;
        st.stat_blade        = _stat_blade;
        return st;
    }

    void loop() {
        if (!_started) return;   // maintenance mode: begin() was skipped
        // Background scan only matters for first-time discovery (no target
        // configured yet). Once a target MAC is stored we connect by address
        // directly — scanning then just steals the radio from connect attempts
        // and spams the log, so skip it. The "Scan Now" UI button still works
        // on demand via trigger_scan().
        if (_target_addr[0] != '\0') return;

        ConnState s = static_cast<ConnState>(_conn_state.load());
        bool connected = (s == ConnState::CONNECTING || s == ConnState::HANDSHAKING ||
                          s == ConnState::AUTHENTICATED);
        if (!connected && !_scanning &&
            ((uint32_t)millis() - _last_scan_ms.load()) >= SCAN_INTERVAL_MS) {
            trigger_scan();
        }
    }
}
