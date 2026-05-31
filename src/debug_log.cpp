// SPDX-License-Identifier: GPL-3.0-or-later
#include "debug_log.h"
#include "config.h"
#include <Arduino.h>
#include <stdarg.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace debug_log {

    struct Entry {
        uint32_t id;
        uint32_t ts;
        Level    level;
        char     src[16];
        char     msg[DEBUG_LOG_LINE_MAX];
    };

    static Entry             _buf[DEBUG_LOG_LINES];
    static uint16_t          _head      = 0;   // next write slot
    static uint32_t          _seq       = 0;   // total entries ever written
    static SemaphoreHandle_t _mutex     = nullptr;
    static broadcast_fn_t    _broadcast = nullptr;

    static const char* TAG[] = {"TRC", "DBG", "INF", "WRN", "ERR"};

    static size_t json_escape(char* dst, size_t dstlen, const char* src) {
        size_t di = 0;
        for (; *src && di + 1 < dstlen; ++src) {
            unsigned char c = (unsigned char)*src;
            if (c == '"' || c == '\\') {
                if (di + 2 >= dstlen) break;
                dst[di++] = '\\';
                dst[di++] = c;
            } else if (c >= 0x20) {
                dst[di++] = (char)c;
            }
        }
        dst[di] = 0;
        return di;
    }

    void begin() {
        _mutex = xSemaphoreCreateMutex();
    }

    void set_broadcast(broadcast_fn_t fn) {
        _broadcast = fn;
    }

    static void write_impl(Level level, const char* source,
                           const char* fmt, va_list ap, bool broadcast) {
        char msg[DEBUG_LOG_LINE_MAX];
        vsnprintf(msg, sizeof(msg), fmt, ap);

        uint32_t ts = (uint32_t)millis();
        const char* lv = TAG[(int)level < 5 ? level : 4];
        const char* src = source ? source : "-";
        Serial.printf("[%lu] %s %s: %s\n", (unsigned long)ts, lv, src, msg);

        if (!_mutex) return;
        if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;

        uint32_t id = _seq++;
        Entry& e    = _buf[_head];
        e.id    = id;
        e.ts    = ts;
        e.level = level;
        strncpy(e.src, src, sizeof(e.src) - 1); e.src[sizeof(e.src) - 1] = 0;
        strncpy(e.msg, msg, sizeof(e.msg) - 1); e.msg[sizeof(e.msg) - 1] = 0;
        _head = (_head + 1) % DEBUG_LOG_LINES;

        xSemaphoreGive(_mutex);

        if (broadcast && _broadcast) {
            char esc[DEBUG_LOG_LINE_MAX * 2];
            json_escape(esc, sizeof(esc), msg);
            char json[DEBUG_LOG_LINE_MAX * 2 + 80];
            snprintf(json, sizeof(json),
                "{\"id\":%lu,\"ts\":%lu,\"lv\":\"%s\",\"src\":\"%s\",\"msg\":\"%s\"}",
                (unsigned long)id, (unsigned long)ts, lv, src, esc);
            _broadcast(json, id);
        }
    }

    void write(Level level, const char* source, const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        write_impl(level, source, fmt, ap, true);
        va_end(ap);
    }

    void write_serial(Level level, const char* source, const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        write_impl(level, source, fmt, ap, false);
        va_end(ap);
    }

    void visit_all(visit_fn_t fn, void* ctx, uint16_t max_recent) {
        if (!_mutex || !fn) return;
        if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return;

        uint32_t total = _seq;
        uint16_t count = (uint16_t)(total < DEBUG_LOG_LINES ? total : DEBUG_LOG_LINES);
        uint16_t start = (uint16_t)(total < DEBUG_LOG_LINES ? 0 : _head);

        // Keep only the most recent max_recent entries (tail), advancing the
        // ring start so a connecting SSE client replays a bounded backlog.
        if (max_recent && count > max_recent) {
            start = (uint16_t)((start + (count - max_recent)) % DEBUG_LOG_LINES);
            count = max_recent;
        }

        Entry* snap = (Entry*)malloc(count * sizeof(Entry));
        if (snap) {
            for (uint16_t i = 0; i < count; i++)
                snap[i] = _buf[(start + i) % DEBUG_LOG_LINES];
        }
        xSemaphoreGive(_mutex);

        if (!snap) return;

        char esc[DEBUG_LOG_LINE_MAX * 2];
        char json[DEBUG_LOG_LINE_MAX * 2 + 80];
        for (uint16_t i = 0; i < count; i++) {
            Entry& e = snap[i];
            json_escape(esc, sizeof(esc), e.msg);
            snprintf(json, sizeof(json),
                "{\"id\":%lu,\"ts\":%lu,\"lv\":\"%s\",\"src\":\"%s\",\"msg\":\"%s\"}",
                (unsigned long)e.id, (unsigned long)e.ts,
                TAG[(int)e.level < 5 ? e.level : 4], e.src, esc);
            fn(json, e.id, ctx);
        }
        free(snap);
    }
}
