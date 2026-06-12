// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

// In-RAM ring buffer of recent log entries.
// write()  → Serial + ring buffer + SSE broadcast (if source set).
// visit_all() → iterate ring buffer oldest→newest (used for SSE replay on connect).

namespace debug_log {
    enum Level { TRACE, DEBUG, INFO, WARN, ERROR };

    // Plain function pointer — no std::function / heap closure needed.
    typedef void (*broadcast_fn_t)(const char* json, uint32_t id);
    typedef void (*visit_fn_t)(const char* json, uint32_t id, void* ctx);

    void begin();
    void write(Level level, const char* source, const char* fmt, ...);
    // Same as write() but does NOT push to the SSE broadcast. Use for hot-path
    // BLE TX/RX dumps so BLE callbacks don't generate live web traffic when the
    // Debug tab is open. Still goes to Serial + ring buffer (replay on connect).
    void write_serial(Level level, const char* source, const char* fmt, ...);

    void set_broadcast(broadcast_fn_t fn); // called on every new entry
    // Replay buffered entries oldest→newest. max_recent>0 limits to the last
    // N entries (bounds the SSE connect-time replay so a fresh client can't
    // flood the AsyncEventSource queue); 0 = all.
    void visit_all(visit_fn_t fn, void* ctx, uint16_t max_recent = 0);
}
