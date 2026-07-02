// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Compile-time defaults. Runtime-changeable values live in NVS (see settings.h).

#define FW_NAME              "flymo-bridge"
#define AP_SSID              "FlymoBridge-setup"
#define MDNS_HOSTNAME        "flymo-bridge"

#define DEFAULT_ADMIN_USER   "admin"
#define DEFAULT_ADMIN_PASS   "admin"

// Pin assignments (ESP32-WROOM-32 DevKit V1)
#define PIN_RESET_BUTTON     0   // on-board BOOT button (GPIO0)
#define PIN_STATUS_LED       2   // on-board blue LED

#define RESET_HOLD_MS        5000

#define WIFI_CONNECT_TIMEOUT_MS  45000
#define BLE_POLL_INTERVAL_MS     8000  // also serves as the session keepalive
#define BLE_RESPONSE_TIMEOUT_MS  10000

#define DEBUG_LOG_LINES      128   // ~27 KB ring (was 256 / ~55 KB)
#define DEBUG_LOG_LINE_MAX   192

// Version source of truth for the ESP-IDF build (there is no platformio.ini
// anymore). Bump here per the CLAUDE.md versioning rule.
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.21.0-dev"
#endif
