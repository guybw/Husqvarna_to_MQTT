// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native ESP-IDF (NimBLE host) port of the Arduino/NimBLE-Arduino reference
// at src/ble/ble_manager.cpp. This is a faithful translation: the handshake
// byte sequences, settle delays, retry counts, the conn_task + semaphore
// request/response pattern, the scan logic, the command queue, the schedule
// read/write, and the full MowerStatus polling/decoding are all preserved.
//
// NimBLE-Arduino C++ classes are replaced with the native NimBLE C host API:
//   - nimble_port_init() + ble_hs_cfg (sync/reset/store) + host task
//   - ble_gap_disc / ble_gap_connect / ble_gap_terminate
//   - ble_gattc_disc_* / ble_gattc_read / ble_gattc_write_*
//   - ble_gap_security_initiate() for Just-Works pairing+bonding
//   - a single gap_event_cb() drives connect/disconnect/notify/enc-change
//
// Async GATT is made to look synchronous for conn_task by kicking an op and
// then blocking on a binary semaphore that the matching callback gives.

#include "ble_manager.h"
#include "automower_protocol.h"
#include "crc8_maxim.h"
#include "debug_log.h"
#include "config.h"
#include "settings.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <atomic>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "nvs_flash.h"

extern "C" {
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "host/ble_hs_id.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_att.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "os/os_mbuf.h"
void ble_store_config_init(void);
}

#define SRC "ble"

// millis() replacement (matches other components: esp_timer_get_time/1000).
static inline uint32_t millis() {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// Chunk size for BLE writes — mower accepts max 20-byte ATT payload;
// use 17 conservatively as documented in protocol-notes.md.
static constexpr size_t BLE_CHUNK = 17;

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

    // ── Native NimBLE link/GATT handles ───────────────────────────────────
    static std::atomic<uint16_t> _conn_handle(BLE_HS_CONN_HANDLE_NONE);
    static std::atomic<bool>     _link_up(false);   // GAP connected
    static uint16_t              _svc_start_handle = 0;
    static uint16_t              _svc_end_handle   = 0;
    static uint16_t              _write_handle     = 0; // 98BD0002 val handle
    static uint16_t              _notify_handle    = 0; // 98BD0003 val handle
    static uint16_t              _devtype_handle   = 0; // 98BD0004 val handle
    static uint16_t              _cccd_handle      = 0; // CCCD of notify char
    static uint16_t              _negotiated_mtu   = 23;

    // Generic GATT-op completion: callbacks fill these then give _gatt_sem.
    static SemaphoreHandle_t _gatt_sem    = nullptr;
    static std::atomic<int>  _gatt_status(0);     // 0 = OK
    static uint8_t           _gatt_read_buf[64];
    static volatile size_t   _gatt_read_len = 0;

    // Connect / security completion semaphores.
    static SemaphoreHandle_t _conn_sem = nullptr; // given by CONNECT event
    static std::atomic<int>  _conn_result(0);
    static SemaphoreHandle_t _enc_sem  = nullptr; // given by ENC_CHANGE event
    static std::atomic<int>  _enc_result(-1);

    // Own address type, inferred once at sync.
    static uint8_t           _own_addr_type = 0;
    static std::atomic<bool> _host_synced(false);

    // ── RX reassembly ─────────────────────────────────────────────────────
    static uint8_t          _rx_buf[512];
    static volatile size_t  _rx_len  = 0;
    static SemaphoreHandle_t _rx_mutex = nullptr;
    static SemaphoreHandle_t _rx_sem   = nullptr; // given by notify_rx

    // ── Polled mower telemetry ────────────────────────────────────────────
    static volatile int16_t  _mower_state    = -1; // -1 = not yet polled
    static volatile int16_t  _mower_activity = -1;
    static volatile int16_t  _mower_battery  = -1;
    static volatile int8_t   _mower_charging   = -1;
    static volatile int32_t  _mower_charge_left = -1;
    static volatile int32_t  _mower_error      = -1;
    static volatile int16_t  _mower_restriction = -1;
    static volatile int32_t  _mower_next_start = -1;
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
    static volatile int16_t  _drive_past       = -1;    // cm  (4712:0)
    static volatile int8_t   _collision_resp   = -1;    // 0/1/2 (4166:11)
    static bool _opt_supported[15];

    // Schedule cache: read once per connected session, published by MQTT while
    // the mower sleeps (read_schedule needs an authenticated link, so we can't
    // read it on demand from the MQTT task).
    static ScheduleTask      _sched_cache[16];
    static volatile uint32_t _sched_count = 0;
    static volatile bool     _sched_valid = false;
    static bool              _sched_read_this_session = false;
    static uint8_t _poll_tick = 0;
    static constexpr uint32_t OPT_QUERY_TIMEOUT_MS = 1500;
    static constexpr uint8_t  OPT_POLL_EVERY       = 5; // ~ every 40 s
    static uint32_t          _last_poll_ms   = 0;

    static bool _started = false;

    // Hard stop for OTA: when set, the conn_task ceases all BLE work (no
    // connect/handshake/poll) so the flash gets the CPU, heap and radio. Not
    // resumed — the device reboots after a successful OTA.
    static std::atomic<bool> _suspended(false);

    // ── Poll-failure watchdog ─────────────────────────────────────────────
    static uint8_t _poll_fail = 0;
    static constexpr uint8_t POLL_FAIL_LIMIT = 3;

    // ── Sleep-respecting connect cadence (manual-wake friendly) ───────────
    // The mower sleeps naturally; we don't fight it. While it rests benignly
    // (docked/charging/parked/restricted) or is unreachable (app layer asleep)
    // we DISCONNECT and only re-check on a long interval — long enough to read
    // battery temperature and notice an autonomous mow, without holding the
    // mower awake. When the mower is reachable and resting we time the next
    // check from its own next-start instead. While it is actually mowing (or
    // stuck / errored) we hold the link and poll every BLE_POLL_INTERVAL_MS so
    // the UI/MQTT track it live until it parks again.
    static uint32_t _next_attempt_ms = 0;   // millis() of next allowed connect
    static uint32_t _hold_until_ms   = 0;   // don't rest before this (millis)
    static uint32_t _idle_recheck_ms = 60UL * 60UL * 1000UL; // runtime (NVS, min)
    static constexpr uint32_t WAKE_MARGIN_MS     = 30000;  // wake 30 s after start
    static constexpr uint32_t HOLD_AFTER_WAKE_MS = 90000;  // live view after Wake
    static constexpr uint32_t HOLD_AFTER_CMD_MS  = 120000; // let a command take
    static std::atomic<bool>  _wake_read_pending(false);
    static std::atomic<bool>  _user_wake(false);   // this attempt is user-driven
    static std::atomic<bool>  _rest_pending(false);// conn_task scheduled the rest

    // ── Pending command (0=none, 1=mow, 2=park, 3=pause) ─────────────────
    static std::atomic<uint8_t>  _pending_cmd(0);
    static std::atomic<uint32_t> _pending_cmd_secs(0);
    static SemaphoreHandle_t _pending_cmd_mutex = nullptr;
    static SemaphoreHandle_t _ble_io_mutex = nullptr;

    // ── Connection task ───────────────────────────────────────────────────
    static TaskHandle_t _conn_task_h = nullptr;

    // Forward decls
    static int gap_event_cb(struct ble_gap_event* event, void* arg);
    static void start_scan_internal();

    // ═════════════════════════════════════════════════════════════════════
    // UUID helpers — native ble_uuid128_t stores bytes REVERSED vs the dashed
    // string ("last hex pair first").
    // ═════════════════════════════════════════════════════════════════════

    // Parse "98bd0001-0b0e-421a-84e5-ddbf75dc6de4" into a ble_uuid128_t.
    static void uuid128_from_str(const char* s, ble_uuid128_t* out) {
        out->u.type = BLE_UUID_TYPE_128;
        // Collect the 16 hex bytes in big-endian (string) order first.
        uint8_t be[16]; int bi = 0;
        for (const char* p = s; *p && bi < 16; ) {
            if (*p == '-') { p++; continue; }
            auto hexv = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return 0;
            };
            be[bi++] = (uint8_t)((hexv(p[0]) << 4) | hexv(p[1]));
            p += 2;
        }
        // NimBLE stores little-endian: reverse.
        for (int i = 0; i < 16; i++) out->value[i] = be[15 - i];
    }

    // Parse "aa:bb:cc:dd:ee:ff" → ble_addr_t.val[] (NimBLE stores LSB first).
    static bool addr_from_str(const char* s, uint8_t type, ble_addr_t* out) {
        unsigned b[6];
        if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x",
                   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
            return false;
        out->type = type;
        for (int i = 0; i < 6; i++) out->val[5 - i] = (uint8_t)b[i];
        return true;
    }

    static void addr_to_str(const ble_addr_t* a, char* out /*≥18*/) {
        snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
                 a->val[5], a->val[4], a->val[3], a->val[2], a->val[1], a->val[0]);
    }

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

    static bool is_connected() {
        return _link_up.load() &&
               _conn_handle.load() != BLE_HS_CONN_HANDLE_NONE;
    }

    // Format a DORMANT detail string: "<prefix> — re-checking in N min", or
    // "<prefix> — manual wake only" when the idle re-check is disabled (0).
    static void recheck_detail(char* out, size_t n, const char* prefix) {
        if (_idle_recheck_ms == 0)
            snprintf(out, n, "%s — manual wake only", prefix);
        else
            snprintf(out, n, "%s — re-checking in %lu min", prefix,
                     (unsigned long)(_idle_recheck_ms / 60000UL));
    }

    static void do_disconnect() {
        uint16_t h = _conn_handle.load();
        if (h != BLE_HS_CONN_HANDLE_NONE)
            ble_gap_terminate(h, BLE_ERR_REM_USER_CONN_TERM);
    }

    static bool write_frame(const uint8_t* data, size_t len) {
        if (!_write_handle || !is_connected()) return false;

        // Hex-dump the first 20 bytes for debugging.
        char hex[64] = {};
        size_t show = len < 20 ? len : 20;
        for (size_t i = 0; i < show; i++)
            snprintf(hex + i * 3, 4, "%02x ", data[i]);
        debug_log::write_serial(debug_log::DEBUG, SRC, "TX %u B: %s%s",
            (unsigned)len, hex, len > show ? "..." : "");

        uint16_t h = _conn_handle.load();
        size_t off = 0;
        while (off < len) {
            size_t chunk = (len - off < BLE_CHUNK) ? (len - off) : BLE_CHUNK;
            // write-without-response, matching the reference's writeValue(...,false)
            int rc = ble_gattc_write_no_rsp_flat(h, _write_handle,
                                                 data + off, (uint16_t)chunk);
            if (rc != 0) {
                debug_log::write(debug_log::ERROR, SRC,
                    "write failed at offset %u (rc=%d)", (unsigned)off, rc);
                return false;
            }
            off += chunk;
            // Pace the controller a touch so back-to-back no-rsp writes don't
            // overrun the ATT queue on slower links.
            vTaskDelay(pdMS_TO_TICKS(5));
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

    // Interruptible delay for the (long) handshake: sleeps in 100 ms steps and
    // bails the instant BLE is suspended (OTA) or the link drops, so a web
    // command / OTA takes effect within ~100 ms instead of up to 5 s. Returns
    // false if the caller should abort the handshake.
    static bool settle(uint32_t ms) {
        for (uint32_t e = 0; e < ms; e += 100) {
            if (_suspended.load() || !is_connected()) return false;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        return !_suspended.load() && is_connected();
    }

    // Build the 24-byte channel-setup link packet (Marbanz protocol.py format).
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
        out[11] = (uint8_t)(ch_id & 0xFF);
        out[12] = (uint8_t)((ch_id >> 8) & 0xFF);
        out[13] = (uint8_t)((ch_id >> 16) & 0xFF);
        out[14] = (uint8_t)(ch_id >> 24);
        out[9]  = crc8::maxim(out + 1, 8);  // hdr_crc = CRC8(bytes[1..8])
        out[24] = crc8::maxim(out + 1, 23); // payload_crc = CRC8(bytes[1..23])
        out[25] = 0x03;
        return 26;
    }

    // Send a query and wait for response. Returns payload byte count in
    // resp_out (up to resp_max bytes). Returns false on timeout/error.
    static bool _send_query(uint16_t major, uint16_t minor,
                            const uint8_t* payload, size_t payload_len,
                            uint8_t* resp_out, size_t resp_max, size_t* resp_len_out,
                            uint32_t timeout_ms = 5000) {
        if (!_ble_io_mutex || xSemaphoreTake(_ble_io_mutex, pdMS_TO_TICKS(5000)) != pdTRUE)
            return false;
        uint8_t buf[64];  // 64 ≥ largest frame (AddTask = 18 hdr + 15 payload + 2)
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
        uint8_t buf[64];  // 64 ≥ largest frame (AddTask = 18 hdr + 15 payload + 2)
        size_t n = automower::encode_request(buf, sizeof(buf),
                                             major, minor, payload, plen);
        if (n == 0) {
            debug_log::write(debug_log::WARN, SRC, "cmd: encode failed");
            xSemaphoreGive(_ble_io_mutex);
            return false;
        }
        bool ok = false;
        for (int attempt = 1; attempt <= 2; attempt++) {
            if (!is_connected()) {
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
        // AddTask (4690/7) payload mirrors the GetTask response layout
        // (confirmed on hardware 2026-06-05): start uint32, duration uint32,
        // then Monday..Sunday flags. (protocol.json's uint16 duration + Sunday-
        // first ordering was wrong — it corrupted duration and dropped Sunday.)
        uint8_t payload[15] = {
            (uint8_t)(task.start & 0xFF),
            (uint8_t)((task.start >> 8) & 0xFF),
            (uint8_t)((task.start >> 16) & 0xFF),
            (uint8_t)((task.start >> 24) & 0xFF),
            (uint8_t)(task.duration & 0xFF),
            (uint8_t)((task.duration >> 8) & 0xFF),
            (uint8_t)((task.duration >> 16) & 0xFF),
            (uint8_t)((task.duration >> 24) & 0xFF),
            (uint8_t)(task.use_on[0] ? 1 : 0),  // Monday
            (uint8_t)(task.use_on[1] ? 1 : 0),  // Tuesday
            (uint8_t)(task.use_on[2] ? 1 : 0),  // Wednesday
            (uint8_t)(task.use_on[3] ? 1 : 0),  // Thursday
            (uint8_t)(task.use_on[4] ? 1 : 0),  // Friday
            (uint8_t)(task.use_on[5] ? 1 : 0),  // Saturday
            (uint8_t)(task.use_on[6] ? 1 : 0),  // Sunday
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
        if (!commit_task_transaction()) return false;
        // Refresh the cache so MQTT/HA reflect the new schedule immediately
        // (instead of waiting for the next connected session to re-read it).
        for (uint32_t i = 0; i < count; i++) _sched_cache[i] = tasks[i];
        _sched_count = count;
        _sched_valid = true;
        return true;
    }

    // Poll GetState, GetActivity, GetBatteryLevel and store in module variables.
    static bool do_status_poll() {
        uint8_t val[4]; size_t vlen = 0;
        if (send_query(4586, 2, val, sizeof(val), &vlen) && vlen >= 1) {
            _mower_state = (int16_t)val[0];
            debug_log::write(debug_log::INFO, SRC, "poll: state=%u", (unsigned)val[0]);
        } else {
            debug_log::write(debug_log::WARN, SRC, "poll: GetState failed");
            return false; // mower not answering — skip the rest, no point
        }
        if (!is_connected()) return false;
        if (send_query(4586, 3, val, sizeof(val), &vlen) && vlen >= 1) {
            _mower_activity = (int16_t)val[0];
            debug_log::write(debug_log::INFO, SRC, "poll: activity=%u", (unsigned)val[0]);
        } else {
            debug_log::write(debug_log::WARN, SRC, "poll: GetActivity failed");
        }
        if (!is_connected()) return false;
        if (send_query(4106, 20, val, sizeof(val), &vlen) && vlen >= 1) {
            _mower_battery = (int16_t)val[0];
            debug_log::write(debug_log::INFO, SRC, "poll: battery=%u%%", (unsigned)val[0]);
        } else {
            debug_log::write(debug_log::WARN, SRC, "poll: GetBatteryLevel failed");
        }

        auto u32le_l = [](const uint8_t* b){
            return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                   ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
        };

        if (!is_connected()) return true;
        if (send_query(4106, 21, val, sizeof(val), &vlen) && vlen >= 1)
            _mower_charging = val[0] ? 1 : 0;

        if (!is_connected()) return true;
        if (send_query(4106, 22, val, sizeof(val), &vlen) && vlen >= 4)
            _mower_charge_left = (int32_t)u32le_l(val);

        if (!is_connected()) return true;
        if (send_query(4586, 6, val, sizeof(val), &vlen) && vlen >= 4) {
            _mower_error = (int32_t)u32le_l(val);
            if (_mower_error != 0)
                debug_log::write(debug_log::WARN, SRC,
                    "poll: error code=%ld", (long)_mower_error);
        }

        if (!is_connected()) return true;
        if (send_query(4658, 0, val, sizeof(val), &vlen) && vlen >= 1)
            _mower_restriction = (int16_t)val[0];

        if (!is_connected()) return true;
        if (send_query(4658, 1, val, sizeof(val), &vlen) && vlen >= 4)
            _mower_next_start = (int32_t)u32le_l(val);

        // ── Outdoor/diagnostic sensors (APK-recovered) ─────────────────────
        if ((_poll_tick++ % OPT_POLL_EVERY) != 0) return true;

        auto s16le = [](const uint8_t* b){
            return (int16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
        };
        uint8_t big[32];

        auto opt = [&](uint8_t i, uint16_t maj, uint16_t min, size_t need) -> bool {
            if (!_opt_supported[i]) return false;
            if (!is_connected()) return false;
            if (send_query(maj, min, big, sizeof(big), &vlen,
                           OPT_QUERY_TIMEOUT_MS) && vlen >= need)
                return true;
            _opt_supported[i] = false;
            debug_log::write(debug_log::WARN, SRC,
                "poll: %u:%u unsupported/slow — disabled for session",
                (unsigned)maj, (unsigned)min);
            return false;
        };

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
            _stat_running    = (int32_t)u32le_l(big +  0);
            _stat_cutting    = (int32_t)u32le_l(big +  4);
            _stat_charging   = (int32_t)u32le_l(big +  8);
            _stat_searching  = (int32_t)u32le_l(big + 12);
            _stat_collisions = (int32_t)u32le_l(big + 16);
            _stat_cycles     = (int32_t)u32le_l(big + 20);
            _stat_blade      = (int32_t)u32le_l(big + 24);
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

        if (opt(13, 4712, 0, 2))                      // DrivePastWire, uint16 cm
            _drive_past = (int16_t)((uint16_t)big[0] | ((uint16_t)big[1] << 8));

        if (opt(14, 4166, 11, 1))                     // collision responsiveness
            _collision_resp = (int8_t)big[0];         // 0=Low 1=Med 2=High

        // Cache the mower's schedule once per session for MQTT to publish.
        if (!_sched_read_this_session) {
            uint32_t cnt = 0;
            if (_read_schedule(_sched_cache, 16, &cnt)) {
                _sched_count = cnt;
                _sched_valid = true;
                debug_log::write(debug_log::INFO, SRC,
                    "schedule cached: %u task(s)", (unsigned)cnt);
            } else {
                debug_log::write(debug_log::WARN, SRC, "schedule read failed");
            }
            _sched_read_this_session = true; // don't retry every poll this session
        }

        return true;
    }

    // ═════════════════════════════════════════════════════════════════════
    // GATT discovery / subscribe / read — async ops made synchronous by
    // blocking on _gatt_sem (given by the matching callback).
    // ═════════════════════════════════════════════════════════════════════

    static bool gatt_wait(uint32_t timeout_ms) {
        return xSemaphoreTake(_gatt_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    }

    static int disc_svc_cb(uint16_t /*conn*/, const struct ble_gatt_error* error,
                           const struct ble_gatt_svc* svc, void* /*arg*/) {
        if (error == nullptr) return 0;
        if (error->status == 0 && svc != nullptr) {
            _svc_start_handle = svc->start_handle;
            _svc_end_handle   = svc->end_handle;
        }
        // Any non-zero status terminates the discovery: BLE_HS_EDONE on normal
        // completion, or a real error (incl. a disconnect mid-discovery). status
        // == 0 means another service record is still coming, so don't signal.
        if (error->status != 0) {
            _gatt_status = (_svc_start_handle != 0) ? 0 : error->status;
            xSemaphoreGive(_gatt_sem);
        }
        return 0;
    }

    static int disc_chr_cb(uint16_t /*conn*/, const struct ble_gatt_error* error,
                           const struct ble_gatt_chr* chr, void* /*arg*/) {
        if (error == nullptr) return 0;
        if (error->status == 0 && chr != nullptr) {
            ble_uuid128_t want;
            uuid128_from_str(automower::CHAR_WRITE_UUID, &want);
            if (ble_uuid_cmp(&chr->uuid.u, &want.u) == 0) _write_handle = chr->val_handle;
            uuid128_from_str(automower::CHAR_NOTIFY_UUID, &want);
            if (ble_uuid_cmp(&chr->uuid.u, &want.u) == 0) _notify_handle = chr->val_handle;
            uuid128_from_str(automower::CHAR_DEVICE_TYPE_UUID, &want);
            if (ble_uuid_cmp(&chr->uuid.u, &want.u) == 0) _devtype_handle = chr->val_handle;
        }
        if (error->status == BLE_HS_EDONE) {
            _gatt_status = 0;
            xSemaphoreGive(_gatt_sem);
        } else if (error->status != 0) {
            _gatt_status = error->status;
            xSemaphoreGive(_gatt_sem);
        }
        return 0;
    }

    static int disc_dsc_cb(uint16_t /*conn*/, const struct ble_gatt_error* error,
                           uint16_t /*chr_val_handle*/,
                           const struct ble_gatt_dsc* dsc, void* /*arg*/) {
        if (error == nullptr) return 0;
        if (error->status == 0 && dsc != nullptr) {
            // CCCD = 0x2902
            const ble_uuid16_t cccd = BLE_UUID16_INIT(BLE_GATT_DSC_CLT_CFG_UUID16);
            if (ble_uuid_cmp(&dsc->uuid.u, &cccd.u) == 0 && _cccd_handle == 0)
                _cccd_handle = dsc->handle;
        }
        if (error->status == BLE_HS_EDONE) {
            _gatt_status = 0;
            xSemaphoreGive(_gatt_sem);
        } else if (error->status != 0) {
            _gatt_status = error->status;
            xSemaphoreGive(_gatt_sem);
        }
        return 0;
    }

    static int read_cb(uint16_t /*conn*/, const struct ble_gatt_error* error,
                       struct ble_gatt_attr* attr, void* /*arg*/) {
        _gatt_read_len = 0;
        if (error != nullptr && error->status == 0 && attr != nullptr && attr->om != nullptr) {
            uint16_t got = 0;
            ble_hs_mbuf_to_flat(attr->om, _gatt_read_buf, sizeof(_gatt_read_buf), &got);
            _gatt_read_len = got;
            _gatt_status = 0;
        } else {
            _gatt_status = error ? error->status : -1;
        }
        xSemaphoreGive(_gatt_sem);
        return 0;
    }

    static int write_dsc_cb(uint16_t /*conn*/, const struct ble_gatt_error* error,
                            struct ble_gatt_attr* /*attr*/, void* /*arg*/) {
        _gatt_status = error ? error->status : -1;
        xSemaphoreGive(_gatt_sem);
        return 0;
    }

    // Discover the mower service + write/notify/devtype characteristics +
    // the notify CCCD handle. Returns false if anything essential is missing.
    static bool discover_gatt() {
        uint16_t h = _conn_handle.load();
        _svc_start_handle = _svc_end_handle = 0;
        _write_handle = _notify_handle = _devtype_handle = _cccd_handle = 0;

        ble_uuid128_t svc_uuid;
        uuid128_from_str(automower::SERVICE_UUID, &svc_uuid);
        xSemaphoreTake(_gatt_sem, 0);
        if (ble_gattc_disc_svc_by_uuid(h, &svc_uuid.u, disc_svc_cb, nullptr) != 0)
            return false;
        if (!gatt_wait(8000) || _svc_start_handle == 0) {
            debug_log::write(debug_log::ERROR, SRC, "service not found");
            return false;
        }

        xSemaphoreTake(_gatt_sem, 0);
        if (ble_gattc_disc_all_chrs(h, _svc_start_handle, _svc_end_handle,
                                    disc_chr_cb, nullptr) != 0)
            return false;
        if (!gatt_wait(8000)) {
            debug_log::write(debug_log::ERROR, SRC, "char discovery timeout");
            return false;
        }
        if (!_write_handle || !_notify_handle) {
            debug_log::write(debug_log::ERROR, SRC, "characteristic not found");
            return false;
        }

        // Find the CCCD of the notify characteristic.
        xSemaphoreTake(_gatt_sem, 0);
        if (ble_gattc_disc_all_dscs(h, _notify_handle, _svc_end_handle,
                                    disc_dsc_cb, nullptr) == 0)
            gatt_wait(8000);
        return true;
    }

    // Subscribe to notifications by writing 0x0001 to the CCCD.
    // with_response: true uses ble_gattc_write_flat (needs encrypted link,
    // matching the reference's subscribe(...,true) readiness semantics);
    // false uses write-no-rsp.
    static bool subscribe_notify(bool with_response) {
        if (!_cccd_handle) {
            debug_log::write(debug_log::WARN, SRC, "no CCCD handle");
            return false;
        }
        uint16_t h = _conn_handle.load();
        uint8_t val[2] = { 0x01, 0x00 }; // notifications enabled
        if (with_response) {
            xSemaphoreTake(_gatt_sem, 0);
            if (ble_gattc_write_flat(h, _cccd_handle, val, sizeof(val),
                                     write_dsc_cb, nullptr) != 0)
                return false;
            if (!gatt_wait(3000)) return false;
            return _gatt_status.load() == 0;
        } else {
            return ble_gattc_write_no_rsp_flat(h, _cccd_handle, val, sizeof(val)) == 0;
        }
    }

    // ═════════════════════════════════════════════════════════════════════
    // GAP event callback — drives connect/disconnect/notify/enc-change.
    // ═════════════════════════════════════════════════════════════════════

    static bool looks_like_mower(const char* name, const uint8_t* svc_uuids_present) {
        if (name) {
            char lc[64]; size_t i = 0;
            for (; name[i] && i < sizeof(lc) - 1; i++)
                lc[i] = (char)tolower((unsigned char)name[i]);
            lc[i] = '\0';
            if (strstr(lc, "husqvarna")) return true;
            if (strstr(lc, "easilife"))  return true;
            if (strstr(lc, "flymo"))     return true;
            if (strstr(lc, "automower")) return true;
            if (strstr(lc, "amtc"))      return true;
        }
        if (svc_uuids_present && *svc_uuids_present) return true;
        return false;
    }

    static void on_disconnect_logic(int /*reason*/) {
        _link_up = false;
        _conn_handle = BLE_HS_CONN_HANDLE_NONE;
        _write_handle = _notify_handle = _devtype_handle = _cccd_handle = 0;
        debug_log::write(debug_log::WARN, SRC, "BLE disconnected");
        // Intentionally DO NOT reset _mower_* telemetry on disconnect.
        xSemaphoreGive(_rx_sem);  // unblock any waiting rx_wait
        xSemaphoreGive(_gatt_sem); // unblock any waiting gatt op
        xSemaphoreGive(_conn_sem);
        xSemaphoreGive(_enc_sem);
        ConnState s = static_cast<ConnState>(_conn_state.load());
        if (s != ConnState::IDLE && s != ConnState::DORMANT) {
            if (_rest_pending.exchange(false)) {
                // conn_task already scheduled _next_attempt_ms and set the
                // detail before disconnecting; just settle into DORMANT.
                _conn_state = static_cast<uint8_t>(ConnState::DORMANT);
            } else {
                // Unexpected drop (mower slept mid-session / link lost). Leave
                // the mower alone and re-check on the long interval.
                _next_attempt_ms = millis() + _idle_recheck_ms;
                char det[64];
                recheck_detail(det, sizeof(det), "disconnected");
                set_state(ConnState::DORMANT, det);
            }
        }
    }

    static int gap_event_cb(struct ble_gap_event* event, void* /*arg*/) {
        switch (event->type) {

        case BLE_GAP_EVENT_DISC: {
            struct ble_hs_adv_fields fields;
            int rc = ble_hs_adv_parse_fields(&fields, event->disc.data,
                                             event->disc.length_data);
            char name[64] = {};
            if (rc == 0 && fields.name != nullptr && fields.name_len > 0) {
                size_t nl = fields.name_len < sizeof(name) - 1
                          ? fields.name_len : sizeof(name) - 1;
                memcpy(name, fields.name, nl);
                name[nl] = '\0';
            }
            // Detect our service UUID in the advertisement.
            uint8_t svc_present = 0;
            if (rc == 0) {
                ble_uuid128_t want; uuid128_from_str(automower::SERVICE_UUID, &want);
                for (int i = 0; i < fields.num_uuids128; i++)
                    if (ble_uuid_cmp(&fields.uuids128[i].u, &want.u) == 0)
                        svc_present = 1;
            }
            char addr[18]; addr_to_str(&event->disc.addr, addr);
            int8_t rssi = event->disc.rssi;
            bool mower = looks_like_mower(name[0] ? name : nullptr, &svc_present);

            if (mower)
                debug_log::write(debug_log::INFO, SRC,
                    "MOWER: %s \"%s\" rssi=%d", addr, name, rssi);
            else
                debug_log::write(debug_log::TRACE, SRC,
                    "dev: %s \"%s\" rssi=%d", addr, name, rssi);

            if (_scan_mutex &&
                xSemaphoreTake(_scan_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                // De-dup by address.
                bool dup = false;
                for (uint8_t i = 0; i < _result_count; i++)
                    if (strcmp(_results[i].addr, addr) == 0) { dup = true; break; }
                if (!dup && _result_count < MAX_RESULTS) {
                    Result& r = _results[_result_count++];
                    strncpy(r.addr, addr, sizeof(r.addr) - 1); r.addr[17] = 0;
                    strncpy(r.name, name, sizeof(r.name) - 1); r.name[63] = 0;
                    r.rssi  = rssi;
                    r.mower = mower;
                    if (mower) {
                        strncpy(_mower_name, name, sizeof(_mower_name) - 1);
                        _mower_rssi = rssi;
                    }
                }
                xSemaphoreGive(_scan_mutex);
            }
            return 0;
        }

        case BLE_GAP_EVENT_DISC_COMPLETE: {
            uint8_t n = 0;
            if (_scan_mutex &&
                xSemaphoreTake(_scan_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                n = _result_count;
                xSemaphoreGive(_scan_mutex);
            }
            _scanning     = false;
            _last_scan_ms = millis();
            debug_log::write(debug_log::INFO, SRC,
                "scan complete — %u device(s)", (unsigned)n);
            return 0;
        }

        case BLE_GAP_EVENT_CONNECT: {
            if (event->connect.status == 0) {
                _conn_handle = event->connect.conn_handle;
                _link_up = true;
                debug_log::write(debug_log::INFO, SRC, "BLE connected");
            } else {
                _link_up = false;
                _conn_handle = BLE_HS_CONN_HANDLE_NONE;
                debug_log::write(debug_log::ERROR, SRC,
                    "connect failed (status=%d)", event->connect.status);
            }
            _conn_result = event->connect.status;
            xSemaphoreGive(_conn_sem);
            return 0;
        }

        case BLE_GAP_EVENT_DISCONNECT:
            on_disconnect_logic(event->disconnect.reason);
            return 0;

        case BLE_GAP_EVENT_ENC_CHANGE:
            debug_log::write(debug_log::INFO, SRC,
                "encryption change status=%d", event->enc_change.status);
            _enc_result = event->enc_change.status;
            xSemaphoreGive(_enc_sem);
            return 0;

        case BLE_GAP_EVENT_NOTIFY_RX: {
            if (!_rx_mutex || !_rx_sem) return 0;
            // Only notifications on our notify char.
            if (_notify_handle && event->notify_rx.attr_handle != _notify_handle)
                return 0;
            uint8_t tmp[256];
            uint16_t got = 0;
            ble_hs_mbuf_to_flat(event->notify_rx.om, tmp, sizeof(tmp), &got);
            if (xSemaphoreTake(_rx_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return 0;

            if (_rx_len + got > sizeof(_rx_buf)) _rx_len = 0; // overflow — reset
            memcpy(_rx_buf + _rx_len, tmp, got);
            _rx_len += got;

            // Classify once we have ≥12 bytes (byte[11]==0xAF → std frame len+4,
            // else handshake/link frame len+2).
            bool complete = false;
            if (_rx_len >= 12 && _rx_buf[0] == 0x02 && _rx_buf[1] == 0xFD) {
                uint16_t frame_len = (uint16_t)(_rx_buf[2] | ((uint16_t)_rx_buf[3] << 8));
                size_t expected = (_rx_buf[11] == 0xAF)
                    ? (size_t)(frame_len + 4)
                    : (size_t)(frame_len + 2);
                if (_rx_len >= expected) { complete = true; _rx_len = expected; }
            }

            char hex[64] = {};
            size_t show = _rx_len < 20 ? _rx_len : 20;
            for (size_t i = 0; i < show; i++)
                snprintf(hex + i * 3, 4, "%02x ", _rx_buf[i]);
            debug_log::write_serial(debug_log::DEBUG, SRC, "RX %u B: %s%s",
                (unsigned)_rx_len, hex, _rx_len > show ? "..." : "");

            xSemaphoreGive(_rx_mutex);
            if (complete) xSemaphoreGive(_rx_sem);
            return 0;
        }

        case BLE_GAP_EVENT_MTU:
            _negotiated_mtu = event->mtu.value;
            debug_log::write(debug_log::INFO, SRC, "MTU negotiated: %u",
                (unsigned)event->mtu.value);
            return 0;

        case BLE_GAP_EVENT_SUBSCRIBE:
            return 0;

        default:
            return 0;
        }
    }

    // ═════════════════════════════════════════════════════════════════════
    // Connection / handshake sequence
    // ═════════════════════════════════════════════════════════════════════

    static bool do_connect_and_handshake(bool do_wake_read) {
        const char* addr = _target_addr;
        if (addr[0] == '\0') return false;
        if (_suspended.load()) return false;   // OTA in progress — don't start

        // Stop scanning before connecting.
        if (_scanning) {
            ble_gap_disc_cancel();
            vTaskDelay(pdMS_TO_TICKS(200));
            _scanning = false;
        }

        set_state(ConnState::CONNECTING, "connecting");
        debug_log::write(debug_log::INFO, SRC, "connecting to %s", addr);

        // Make sure no stale link exists.
        if (is_connected()) {
            do_disconnect();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        _link_up = false;
        _conn_handle = BLE_HS_CONN_HANDLE_NONE;

        // Try public address type first, then random.
        bool connected = false;
        for (int variant = 0; variant < 2 && !connected; variant++) {
            // A CONNECT event from the previous variant can land just after its
            // wait timed out; if we're already up, don't fire a second connect
            // (which would return BLE_HS_EALREADY and wedge the attempt).
            if (is_connected()) { connected = true; break; }
            ble_addr_t peer;
            uint8_t type = (variant == 0) ? BLE_ADDR_PUBLIC : BLE_ADDR_RANDOM;
            if (!addr_from_str(addr, type, &peer)) {
                debug_log::write(debug_log::ERROR, SRC, "bad MAC string");
                return false;
            }
            xSemaphoreTake(_conn_sem, 0);
            int rc = ble_gap_connect(_own_addr_type, &peer, 30000, nullptr,
                                     gap_event_cb, nullptr);
            if (rc != 0) {
                debug_log::write(debug_log::WARN, SRC,
                    "ble_gap_connect rc=%d (variant %d)", rc, variant);
                continue;
            }
            // Wait for the CONNECT event.
            if (xSemaphoreTake(_conn_sem, pdMS_TO_TICKS(31000)) == pdTRUE &&
                _conn_result.load() == 0 && is_connected()) {
                connected = true;
            } else {
                // make sure we don't leak a half-open connect attempt
                ble_gap_conn_cancel();
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
        if (!connected) {
            debug_log::write(debug_log::ERROR, SRC, "connect failed");
            return false;
        }

        // Match the Android app's requestMtu(517): explicitly initiate an MTU
        // exchange right after connect — ideally BEFORE the mower initiates its
        // own (small) one, since the mower expects a large MTU before it will
        // echo the channel setup. rc != 0 (e.g. BLE_HS_EALREADY) means an
        // exchange already happened; the negotiated value is logged on
        // BLE_GAP_EVENT_MTU.
        int xmtu_rc = ble_gattc_exchange_mtu(_conn_handle.load(), nullptr, nullptr);
        debug_log::write(debug_log::INFO, SRC, "exchange_mtu rc=%d", xmtu_rc);

        // Secure the connection immediately after connecting, before any GATT
        // operations. The mower opens a short pairing window after the user
        // enters the PIN physically; attempting security as early as possible
        // maximises the chance of catching that window. Bonding is enabled so
        // the keys persist in NVS for subsequent reconnects.
        debug_log::write(debug_log::INFO, SRC, "securing connection...");
        _enc_result = -1;
        xSemaphoreTake(_enc_sem, 0);
        int sec_rc = ble_gap_security_initiate(_conn_handle.load());
        if (sec_rc == 0 &&
            xSemaphoreTake(_enc_sem, pdMS_TO_TICKS(8000)) == pdTRUE &&
            _enc_result.load() == 0) {
            debug_log::write(debug_log::INFO, SRC, "connection secured");
        } else {
            debug_log::write(debug_log::WARN, SRC,
                "secureConnection() failed (rc=%d enc=%d) — continuing anyway",
                sec_rc, _enc_result.load());
        }
        if (!is_connected()) return false;

        // Discover service and characteristics (+ notify CCCD).
        if (!discover_gatt()) {
            if (is_connected()) do_disconnect();
            return false;
        }

        // Read the device-type characteristic (98BD0004) before subscribing to
        // wake the mower's GATT application layer (explicit Wake / pairing only).
        if (do_wake_read) {
            if (_devtype_handle) {
                xSemaphoreTake(_gatt_sem, 0);
                _gatt_read_len = 0;
                if (ble_gattc_read(_conn_handle.load(), _devtype_handle,
                                   read_cb, nullptr) == 0 && gatt_wait(3000))
                    debug_log::write(debug_log::INFO, SRC,
                        "device type read (%u bytes) — app layer woken",
                        (unsigned)_gatt_read_len);
                else
                    debug_log::write(debug_log::WARN, SRC, "device type read failed");
            } else {
                debug_log::write(debug_log::WARN, SRC, "device type char not found");
            }
            if (!settle(2000)) { if (is_connected()) do_disconnect(); return false; }
        } else {
            debug_log::write(debug_log::INFO, SRC,
                "passive connect — skipping wake-read (won't wake a sleeping mower)");
            if (!settle(500)) { if (is_connected()) do_disconnect(); return false; }
        }

        // Subscribe to notifications. with-response succeeds only on an
        // encrypted link (readiness indicator); fall back to no-response and
        // continue into phase 1 as the real readiness test.
        bool sub_with_resp = subscribe_notify(true);
        if (sub_with_resp) {
            debug_log::write(debug_log::INFO, SRC, "subscribed (with-response)");
        } else {
            debug_log::write(debug_log::WARN, SRC,
                "subscribe (with-response) failed");
            if (subscribe_notify(false)) {
                debug_log::write(debug_log::WARN, SRC,
                    "subscribed (no-response) — proceeding to phase 1 to confirm");
            } else {
                debug_log::write(debug_log::ERROR, SRC, "subscribe failed (both modes)");
                if (is_connected()) do_disconnect();
                return false;
            }
        }

        debug_log::write(debug_log::INFO, SRC,
            "GATT service+chars found, notifications subscribed");
        debug_log::write(debug_log::INFO, SRC, "MTU negotiated: %u",
            (unsigned)_negotiated_mtu);

        // Give the mower 5 s to settle AFTER subscription before sending frames
        // (interruptible — an OTA suspend or dropped link aborts immediately).
        if (!settle(5000)) { if (is_connected()) do_disconnect(); return false; }

        set_state(ConnState::HANDSHAKING, "handshake");

        auto log_rx = [](const char* label, const uint8_t* buf, size_t n) {
            char hex[100] = {};
            size_t show = n < 20 ? n : 20;
            for (size_t i = 0; i < show; i++) snprintf(hex + i * 3, 4, "%02x ", buf[i]);
            debug_log::write(debug_log::INFO, SRC, "%s %u B: %s%s",
                label, (unsigned)n, hex, n > show ? "..." : "");
        };

        // ── Phase 1: channel-setup link packet (reuse persisted channel ID) ─
        uint32_t ch_id = settings::get_channel_id();
        if (ch_id == 0) {
            ch_id = esp_random();
            if (ch_id == 0) ch_id = 1;
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

        bool phase1_ok = false;
        for (int attempt = 0; attempt < 3 && !phase1_ok; attempt++) {
            if (attempt > 0) {
                if (!is_connected() || _suspended.load()) break;
                debug_log::write(debug_log::INFO, SRC,
                    "HS phase1: retry %d (waiting 2s)", attempt + 1);
                if (!settle(2000)) break;
            }
            rx_flush();
            if (!write_frame(ch_pkt, sizeof(ch_pkt))) {
                debug_log::write(debug_log::ERROR, SRC, "HS phase1: write failed");
                if (is_connected()) do_disconnect();
                return false;
            }
            if (rx_wait(5000)) {
                if (!is_connected()) {
                    debug_log::write(debug_log::WARN, SRC,
                        "HS phase1: disconnected during wait");
                    return false;
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
            if (is_connected()) {
                debug_log::write(debug_log::WARN, SRC,
                    "HS phase1: all attempts failed — mower not ready, retrying");
                do_disconnect();
            }
            return false;
        }

        // ── Phase 2: handshake-confirmation bytes ──────────────────────────
        debug_log::write(debug_log::INFO, SRC, "HS phase2: sending handshake");
        uint8_t hs_pkt[14];
        build_handshake(hs_pkt, ch_id);
        rx_flush();
        if (!write_frame(hs_pkt, sizeof(hs_pkt))) {
            debug_log::write(debug_log::ERROR, SRC, "HS phase2: write failed");
            if (is_connected()) do_disconnect();
            return false;
        }
        if (rx_wait(3000)) {
            uint8_t buf[128]; size_t n = rx_take(buf, sizeof(buf));
            log_rx("HS phase2 resp", buf, n);
        } else {
            debug_log::write(debug_log::INFO, SRC, "HS phase2: no response (ok)");
        }

        // ── Phase 3: EnterOperatorPin ──────────────────────────────────────
        set_state(ConnState::HANDSHAKING, "EnterOperatorPin");
        debug_log::write(debug_log::INFO, SRC,
            "HS phase3: PIN=%u", (unsigned)_target_pin);
        rx_flush();
        if (!send_enter_pin(_target_pin)) {
            debug_log::write(debug_log::ERROR, SRC, "HS phase3: write failed");
            if (is_connected()) do_disconnect();
            return false;
        }
        if (!rx_wait(BLE_RESPONSE_TIMEOUT_MS)) {
            debug_log::write(debug_log::ERROR, SRC, "HS phase3: PIN response timeout");
            if (is_connected()) do_disconnect();
            return false;
        }
        // Guard against the disconnect-unblocking-rx_sem race.
        if (!is_connected()) {
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

        // ── Authenticated — initial status poll immediately ────────────────
        _poll_fail = 0;
        for (auto& s : _opt_supported) s = true;
        _poll_tick = 0;
        _sched_read_this_session = false;
        set_state(ConnState::AUTHENTICATED, "authenticated");

        _last_poll_ms = millis();
        do_status_poll();
        return true;
    }

    // True when the mower is sitting safely at rest and we can let it sleep.
    // Mowing / leaving / returning / paused / stuck / errored all return false
    // so the conn_task keeps the link up and tracks it live.
    static bool mower_is_benign_resting() {
        if (_mower_state < 0 || _mower_activity < 0) return false; // not polled
        if (_mower_error > 0) return false;                        // problem
        int a = _mower_activity, st = _mower_state;
        bool act_ok = (a == 0 /*NONE*/ || a == 1 /*CHARGING*/ || a == 5 /*PARKED*/);
        bool st_ok  = (st == 0 /*OFF*/ || st == 2 /*STOPPED*/ || st == 7 /*RESTRICTED*/);
        return act_ok && st_ok;
    }

    // Read the mower's own clock (GetTime 4690:2, uint32). Used to convert the
    // absolute next-start timestamp into a wake delay without needing the
    // ESP32's wall clock. Returns false if unavailable.
    static bool get_mower_time(uint32_t& out) {
        uint8_t v[4]; size_t n = 0;
        if (send_query(4690, 2, v, sizeof(v), &n) && n >= 4) {
            out = (uint32_t)v[0] | ((uint32_t)v[1] << 8) |
                  ((uint32_t)v[2] << 16) | ((uint32_t)v[3] << 24);
            return true;
        }
        return false;
    }

    // Disconnect and schedule the next connect. If the mower advertised a
    // next-start sooner than _idle_recheck_ms, wake just after it; otherwise
    // re-check on the long interval (which also covers battery-temp polling).
    static void enter_rest() {
        uint32_t delay_ms = _idle_recheck_ms;
        // Only time the next check from the mower's next-start when auto-recheck
        // is enabled (0 = manual-only: never auto-wake, not even at next-start).
        if (_idle_recheck_ms != 0) {
            int32_t ns = _mower_next_start;
            if (ns > 0) {
                uint32_t mnow = 0;
                if (get_mower_time(mnow) && (uint32_t)ns > mnow) {
                    uint32_t d_s = (uint32_t)ns - mnow;
                    if (d_s * 1000UL < _idle_recheck_ms)
                        delay_ms = d_s * 1000UL + WAKE_MARGIN_MS;
                }
            }
        }
        _next_attempt_ms = millis() + delay_ms;
        char det[80];
        if (_idle_recheck_ms == 0)
            snprintf(det, sizeof(det), "resting — manual wake only");
        else if (delay_ms < _idle_recheck_ms)
            snprintf(det, sizeof(det), "resting — next start in ~%lu min",
                     (unsigned long)(delay_ms / 60000UL));
        else
            snprintf(det, sizeof(det), "resting — re-checking in %lu min",
                     (unsigned long)(_idle_recheck_ms / 60000UL));
        _rest_pending = true;
        debug_log::write(debug_log::INFO, SRC,
            "benign rest — disconnecting (%s)",
            _idle_recheck_ms == 0 ? "manual wake only" : "auto re-check scheduled");
        set_state(ConnState::DORMANT, det);
        if (is_connected()) do_disconnect();
    }

    // ═════════════════════════════════════════════════════════════════════
    // Connection task
    // ═════════════════════════════════════════════════════════════════════

    static void conn_task_fn(void*) {
        // Wait for the NimBLE host to sync before doing any GAP work.
        while (!_host_synced.load()) vTaskDelay(pdMS_TO_TICKS(50));

        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(500));

            // OTA in progress: do nothing (no connect/handshake/poll/radio).
            if (_suspended.load()) continue;

            ConnState s = static_cast<ConnState>(_conn_state.load());
            uint32_t  now = millis();

            // Again-asleep / failed connect → re-check on the long interval
            // instead of churning every couple of minutes.
            auto sleep_off = [&](const char* prefix) {
                _next_attempt_ms = millis() + _idle_recheck_ms;
                char det[64];
                recheck_detail(det, sizeof(det), prefix);
                set_state(ConnState::DORMANT, det);
            };

            if (s == ConnState::ERROR) {
                sleep_off("error");
                continue;
            }

            // DORMANT and IDLE both mean "try to connect": DORMANT waits for its
            // scheduled _next_attempt_ms; IDLE (a fresh user/target request)
            // goes immediately.
            if (s == ConnState::DORMANT || s == ConnState::IDLE) {
                if (_target_addr[0] == '\0') continue;
                if (_scanning) continue;
                // recheck == 0 → manual-only: DORMANT never auto-connects. Only a
                // Wake/command (which moves us to IDLE) leaves DORMANT.
                if (s == ConnState::DORMANT && _idle_recheck_ms == 0) continue;
                if (s == ConnState::DORMANT &&
                    (int32_t)(now - _next_attempt_ms) < 0) continue;
                bool wake = _wake_read_pending.exchange(false);
                bool user = _user_wake.exchange(false);
                if (do_connect_and_handshake(wake)) {
                    // Hold the link briefly after a user Wake so the UI shows
                    // live data; an automatic re-check rests as soon as it has
                    // read the mower (see the AUTHENTICATED branch).
                    _hold_until_ms = millis() + (user ? HOLD_AFTER_WAKE_MS : 0);
                } else {
                    sleep_off("asleep");
                }
                continue;
            }

            if (s == ConnState::AUTHENTICATED) {
                uint8_t cmd = 0;
                uint32_t cmd_secs = 0;
                if (_pending_cmd_mutex &&
                    xSemaphoreTake(_pending_cmd_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
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
                        major = 5370; minor = 2;
                        cmd_name = (cmd == 4) ? "frost_on" : "frost_off";
                        payload[0] = (cmd == 4) ? 1 : 0;
                        plen = 1;
                    }
                    else if (cmd == 6 || cmd == 7) {
                        major = 4692; minor = 3;
                        cmd_name = (cmd == 6) ? "garage_on" : "garage_off";
                        payload[0] = (cmd == 6) ? 1 : 0;
                        plen = 1;
                    }
                    else if (cmd == 8) {
                        major = 4460; minor = 5; cmd_name = "lawn_off";
                        payload[0] = 0; plen = 1;
                    }
                    else if (cmd == 12) {             // SetDrivePastWire 4712:1
                        major = 4712; minor = 1; cmd_name = "drive_past";
                        payload[0] = (uint8_t)(cmd_secs & 0xFF);
                        payload[1] = (uint8_t)((cmd_secs >> 8) & 0xFF);
                        plen = 2;
                        debug_log::write(debug_log::INFO, SRC,
                            "cmd: drive-past-wire %lu cm", (unsigned long)cmd_secs);
                    }
                    else if (cmd >= 13 && cmd <= 15) { // SetResponsiveness 4166:12
                        major = 4166; minor = 12;
                        cmd_name = (cmd == 13) ? "collision_low"
                                 : (cmd == 14) ? "collision_med" : "collision_high";
                        payload[0] = (uint8_t)(cmd - 13); // 0=Low 1=Med 2=High
                        plen = 1;
                    }
                    else if (cmd >= 9 && cmd <= 11) {
                        uint8_t sens = (uint8_t)(cmd - 8); // 9→1,10→2,11→3
                        uint8_t en = 1;
                        const char* nm = (cmd == 9) ? "lawn_low"
                                       : (cmd == 10) ? "lawn_med" : "lawn_high";
                        debug_log::write(debug_log::INFO, SRC, "cmd: sending %s", nm);
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
                    // Hold the link open after a command so its effect is
                    // visible, and re-poll immediately to pick up the change.
                    _hold_until_ms = millis() + HOLD_AFTER_CMD_MS;
                    _last_poll_ms  = 0;
                }
                uint32_t now2 = millis();
                if (now2 - _last_poll_ms >= (uint32_t)BLE_POLL_INTERVAL_MS) {
                    _last_poll_ms = now2;
                    if (do_status_poll()) {
                        _poll_fail = 0;
                    } else if (++_poll_fail >= POLL_FAIL_LIMIT) {
                        _next_attempt_ms = millis() + _idle_recheck_ms;
                        _rest_pending = true;
                        char det[64];
                        recheck_detail(det, sizeof(det), "mower slept");
                        debug_log::write(debug_log::INFO, SRC, "%s", det);
                        set_state(ConnState::DORMANT, det);
                        if (is_connected()) do_disconnect();
                    }
                }

                // Rest decision: once the mower is back to a benign resting
                // state (docked/charging/parked/restricted, no error) and any
                // post-wake / post-command hold has elapsed, disconnect and let
                // it sleep — scheduling the next check from its own next-start.
                if (is_authenticated() && is_connected() &&
                    (int32_t)(millis() - _hold_until_ms) >= 0 &&
                    mower_is_benign_resting()) {
                    enter_rest();
                }
            }
        }
    }

    // ═════════════════════════════════════════════════════════════════════
    // NimBLE host bring-up
    // ═════════════════════════════════════════════════════════════════════

    static void on_host_sync() {
        // Ensure we have an identity address and infer own_addr_type.
        ble_hs_util_ensure_addr(0);
        int rc = ble_hs_id_infer_auto(0, &_own_addr_type);
        if (rc != 0) {
            debug_log::write(debug_log::ERROR, SRC,
                "ble_hs_id_infer_auto rc=%d", rc);
            _own_addr_type = BLE_OWN_ADDR_PUBLIC;
        }
        _host_synced = true;
        debug_log::write(debug_log::INFO, SRC,
            "NimBLE host synced (own_addr_type=%u)", (unsigned)_own_addr_type);
    }

    static void on_host_reset(int reason) {
        debug_log::write(debug_log::WARN, SRC, "NimBLE host reset; reason=%d", reason);
        _host_synced = false;
        _link_up = false;
        _conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }

    static void host_task(void* /*param*/) {
        // This blocks until nimble_port_stop() runs.
        nimble_port_run();
        nimble_port_freertos_deinit();
    }

    static void start_scan_internal() {
        struct ble_gap_disc_params dp = {};
        dp.itvl = 100;          // 0.625ms units
        dp.window = 99;
        dp.passive = 0;         // active scan
        dp.filter_duplicates = 1;
        dp.limited = 0;
        int rc = ble_gap_disc(_own_addr_type, SCAN_DURATION_S * 1000, &dp,
                              gap_event_cb, nullptr);
        if (rc != 0) {
            _scanning = false;
            debug_log::write(debug_log::WARN, SRC, "ble_gap_disc rc=%d", rc);
        } else {
            debug_log::write(debug_log::INFO, SRC,
                "BLE scan started (%us)", (unsigned)SCAN_DURATION_S);
        }
    }

    // ═════════════════════════════════════════════════════════════════════
    // Public API
    // ═════════════════════════════════════════════════════════════════════

    void begin() {
        _scan_mutex = xSemaphoreCreateMutex();
        _rx_mutex   = xSemaphoreCreateMutex();
        _rx_sem     = xSemaphoreCreateBinary();
        _gatt_sem   = xSemaphoreCreateBinary();
        _conn_sem   = xSemaphoreCreateBinary();
        _enc_sem    = xSemaphoreCreateBinary();
        _pending_cmd_mutex = xSemaphoreCreateMutex();
        _ble_io_mutex = xSemaphoreCreateMutex();

        // NVS must be ready for the NimBLE bond store. settings::begin()
        // already inits NVS, but guard in case begin() is called standalone.
        esp_err_t nrc = nvs_flash_init();
        if (nrc == ESP_ERR_NVS_NO_FREE_PAGES || nrc == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            nvs_flash_erase();
            nvs_flash_init();
        }

        if (nimble_port_init() != ESP_OK) {
            debug_log::write(debug_log::ERROR, SRC, "nimble_port_init failed");
            return;
        }

        // Host config: sync/reset/store callbacks + security.
        ble_hs_cfg.sync_cb         = on_host_sync;
        ble_hs_cfg.reset_cb        = on_host_reset;
        ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

        // Security: Just Works pairing (no IO cap, no MITM, SC=yes), bonding on.
        ble_hs_cfg.sm_io_cap        = BLE_HS_IO_NO_INPUT_OUTPUT;
        ble_hs_cfg.sm_bonding       = 1;
        ble_hs_cfg.sm_sc            = 1;
        ble_hs_cfg.sm_mitm          = 0;
        ble_hs_cfg.sm_our_key_dist  = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
        ble_hs_cfg.sm_their_key_dist= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

        // NVS-backed bond store.
        ble_store_config_init();

        // Prefer MTU 517 to match the Android app behaviour (mower appears to
        // require MTU negotiation before echoing the channel setup). Log the
        // result — if this fails, the host falls back to the compile-time
        // CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU default.
        int mtu_rc = ble_att_set_preferred_mtu(517);
        debug_log::write(debug_log::INFO, SRC,
            "ble_att_set_preferred_mtu(517) rc=%d", mtu_rc);

        // Run the NimBLE host on its own task.
        nimble_port_freertos_init(host_task);

        // Load the idle re-check interval (minutes → ms; 0 = manual-only).
        {
            uint32_t m = settings::get_idle_recheck_min();
            if (m > 1440) m = 1440;
            _idle_recheck_ms = m * 60000UL;
            debug_log::write(debug_log::INFO, SRC,
                "idle re-check interval: %u min%s", (unsigned)m,
                m == 0 ? " (manual only)" : "");
        }

        // Load stored target from NVS.
        std::string mac = settings::get_mower_mac();
        if (!mac.empty()) {
            strncpy(_target_addr, mac.c_str(), sizeof(_target_addr) - 1);
            _target_pin = settings::get_mower_pin();
            uint32_t saved_ch = settings::get_channel_id();
            debug_log::write(debug_log::INFO, SRC,
                "target loaded from NVS: %s pin=%u ch_id=0x%08x",
                _target_addr, (unsigned)_target_pin, (unsigned)saved_ch);
            set_state(ConnState::DORMANT, _idle_recheck_ms == 0
                ? "manual wake only — press Wake"
                : "boot — connecting once");
        }

        // Start connection task (stack 6144 bytes, priority 1).
        xTaskCreate(conn_task_fn, "ble_conn", 6144, nullptr, 1, &_conn_task_h);

        _started = true;
        debug_log::write(debug_log::INFO, SRC,
            "NimBLE ready (sleep-respecting cadence; Wake = connect now)");
    }

    void trigger_scan() {
        if (!_started) return;
        if (!_host_synced.load()) return;
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
        start_scan_internal();
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
        if (!_started) return;
        bool mac_changed = (settings::get_mower_mac() != std::string(mac));
        settings::set_mower_mac(std::string(mac));
        settings::set_mower_pin(pin);
        if (mac_changed) {
            settings::set_channel_id(0);
            debug_log::write(debug_log::INFO, SRC, "new mower MAC — channel ID reset");
        }
        strncpy(_target_addr, mac, sizeof(_target_addr) - 1);
        _target_addr[sizeof(_target_addr) - 1] = '\0';
        _target_pin = pin;

        ConnState s = static_cast<ConnState>(_conn_state.load());

        if ((s == ConnState::CONNECTING || s == ConnState::HANDSHAKING) &&
            strncmp(_target_addr, mac, sizeof(_target_addr)) == 0) {
            debug_log::write(debug_log::INFO, SRC,
                "target pin updated to %u (connection already in progress)", (unsigned)pin);
            return;
        }

        if (s != ConnState::IDLE && s != ConnState::ERROR &&
            s != ConnState::DORMANT) {
            if (is_connected()) do_disconnect();
        }

        _wake_read_pending = true;
        _user_wake         = true;
        _next_attempt_ms   = millis();
        set_state(ConnState::IDLE, "new target set");
        debug_log::write(debug_log::INFO, SRC,
            "target set: %s pin=%u", mac, (unsigned)pin);
    }

    void disconnect_target() {
        if (!_started) return;
        if (is_connected()) do_disconnect();
        _target_addr[0] = '\0';
        settings::set_mower_mac(std::string(""));
        set_state(ConnState::IDLE, "disconnected by user");
        debug_log::write(debug_log::INFO, SRC, "disconnected by user");
    }

    // Stop ALL BLE activity for an OTA flash (frees CPU + radio; the conn_task
    // idles on _suspended). Drops any live link and cancels scanning, but keeps
    // the stored target so a normal boot after the OTA reconnects as usual.
    // One-way: the device reboots after a successful OTA, which re-inits BLE.
    void suspend() {
        if (!_started) return;
        _suspended = true;
        debug_log::write(debug_log::WARN, SRC,
            "BLE suspended for OTA — ceasing all activity");
        if (_scanning) { ble_gap_disc_cancel(); _scanning = false; }
        if (is_connected()) do_disconnect();
    }

    void force_wake() {
        if (!_started) return;
        if (_target_addr[0] == '\0') {
            debug_log::write(debug_log::WARN, SRC,
                "wake requested but no target configured");
            return;
        }
        ConnState s = static_cast<ConnState>(_conn_state.load());

        if (s == ConnState::AUTHENTICATED) {
            // Already connected — extend the live-view hold so the UI keeps
            // updating for a bit instead of resting immediately.
            _hold_until_ms = millis() + HOLD_AFTER_WAKE_MS;
            debug_log::write(debug_log::INFO, SRC,
                "wake — already authenticated; holding link for live view");
            return;
        }

        _poll_fail         = 0;
        _wake_read_pending = true;
        _user_wake         = true;
        _next_attempt_ms   = millis();

        if (s == ConnState::CONNECTING || s == ConnState::HANDSHAKING) {
            debug_log::write(debug_log::INFO, SRC,
                "wake requested — aborting in-flight passive attempt");
            if (is_connected()) do_disconnect();
            return;
        }

        set_state(ConnState::IDLE, "wake requested — connecting");
        debug_log::write(debug_log::INFO, SRC,
            "wake requested via API — attempting connection (wake-read)");
    }

    void set_idle_recheck_minutes(uint32_t minutes) {
        if (minutes > 1440) minutes = 1440;   // 0 = manual-only (never auto-wake)
        settings::set_idle_recheck_min(minutes);
        _idle_recheck_ms = minutes * 60000UL;
        debug_log::write(debug_log::INFO, SRC,
            "idle re-check interval set to %u min%s", (unsigned)minutes,
            minutes == 0 ? " (manual only)" : "");
    }

    uint32_t get_idle_recheck_minutes() {
        return _idle_recheck_ms / 60000UL;
    }

    bool queue_command(const char* cmd, uint32_t secs) {
        if (!_started) return false;
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
        else if (strcmp(cmd, "drivepast") == 0) c = 12;  // secs = distance cm
        else if (strcmp(cmd, "collision_low")  == 0) c = 13;
        else if (strcmp(cmd, "collision_med")  == 0) c = 14;
        else if (strcmp(cmd, "collision_high") == 0) c = 15;
        else return false;
        if (_pending_cmd_mutex &&
            xSemaphoreTake(_pending_cmd_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            _pending_cmd_secs = secs;
            _pending_cmd = c;
            xSemaphoreGive(_pending_cmd_mutex);
        } else {
            _pending_cmd.store(c);
            _pending_cmd_secs = secs;
        }
        debug_log::write(debug_log::INFO, SRC, "command queued: %s", cmd);
        // If the mower is asleep, wake it so the command/setting is applied
        // promptly; it returns to its normal rest cadence afterwards.
        if (static_cast<ConnState>(_conn_state.load()) != ConnState::AUTHENTICATED)
            force_wake();
        return true;
    }

    bool read_schedule(ScheduleTask* out, uint32_t max_out, uint32_t* out_count) {
        return _read_schedule(out, max_out, out_count);
    }

    bool write_schedule(const ScheduleTask* tasks, uint32_t count) {
        return _write_schedule(tasks, count);
    }

    bool write_schedule_wake(const ScheduleTask* tasks, uint32_t count) {
        // If the mower slept (e.g. editing took longer than the wake-hold), wake
        // it and wait for the link before writing. Blocks the caller up to ~45 s
        // — intended for the web Save handler (a deliberate user action).
        if (!is_authenticated()) {
            debug_log::write(debug_log::INFO, SRC, "schedule write: waking mower");
            force_wake();
            for (int i = 0; i < 90 && !is_authenticated(); i++)
                vTaskDelay(pdMS_TO_TICKS(500));
            if (!is_authenticated()) {
                debug_log::write(debug_log::WARN, SRC,
                    "schedule write: mower did not wake in time");
                return false;
            }
        }
        return _write_schedule(tasks, count);
    }

    bool get_schedule_cache(ScheduleTask* out, uint32_t max_out, uint32_t* out_count) {
        if (!out || !out_count || !_sched_valid) return false;
        uint32_t n = _sched_count < max_out ? _sched_count : max_out;
        for (uint32_t i = 0; i < n; i++) out[i] = _sched_cache[i];
        *out_count = n;
        return true;
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
        st.drive_past        = _drive_past;
        st.collision_resp    = _collision_resp;
        return st;
    }

    void loop() {
        // BLE scanning is strictly ON-DEMAND. The web UI "Scan for mowers"
        // button (POST /api/ble/trigger -> trigger_scan()) drives discovery, and
        // connecting to a paired mower happens via set_target()/force_wake() on
        // the conn_task. We deliberately do NOT run a periodic background
        // discovery scan: BLE and WiFi share one radio, so an unsolicited scan
        // starves WiFi (AP beacons, STA network scan, web page loads) — most
        // visibly during first-time setup. This intentionally departs from the
        // Arduino reference, which auto-scanned every 30 s while no mower was
        // paired. Desired flow: connect to dongle -> set up WiFi -> the user
        // chooses when to scan for mowers.
    }
}
