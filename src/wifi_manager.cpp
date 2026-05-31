// SPDX-License-Identifier: GPL-3.0-or-later
#include "wifi_manager.h"
#include "config.h"
#include "settings.h"
#include "debug_log.h"
#include "status_led.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <DNSServer.h>

#define SRC "wifi"

namespace wifi_manager {

    static bool      _connected = false;
    static bool      _portal    = false;
    static DNSServer _dns;

    static void start_ap() {
        _portal = true;
        WiFi.mode(WIFI_AP);
        WiFi.softAP(AP_SSID);
        IPAddress ip = WiFi.softAPIP();
        _dns.start(53, "*", ip);
        debug_log::write(debug_log::INFO, SRC,
            "AP mode  ssid=%s  ip=%s", AP_SSID, ip.toString().c_str());
        status_led::set(status_led::BLINK_SLOW);
    }

    void begin() {
        String ssid = settings::get_wifi_ssid();

        if (ssid.isEmpty()) {
            debug_log::write(debug_log::INFO, SRC, "no saved SSID — starting AP");
            start_ap();
            return;
        }

        debug_log::write(debug_log::INFO, SRC, "STA connecting ssid=%s", ssid.c_str());
        status_led::set(status_led::BLINK_FAST);
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(true);
        WiFi.begin(ssid.c_str(), settings::get_wifi_psk().c_str());

        uint32_t deadline = millis() + WIFI_CONNECT_TIMEOUT_MS;
        while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
            delay(200);
        }

        if (WiFi.status() == WL_CONNECTED) {
            _connected = true;
            debug_log::write(debug_log::INFO, SRC,
                "STA connected  ip=%s", WiFi.localIP().toString().c_str());
            if (MDNS.begin(MDNS_HOSTNAME)) {
                debug_log::write(debug_log::INFO, SRC,
                    "mDNS ready: %s.local", MDNS_HOSTNAME);
            } else {
                debug_log::write(debug_log::WARN, SRC, "mDNS begin failed");
            }
            status_led::set(status_led::SOLID);
        } else {
            debug_log::write(debug_log::WARN, SRC,
                "STA timeout after %u ms — falling back to AP", WIFI_CONNECT_TIMEOUT_MS);
            WiFi.disconnect(true);
            delay(100);
            start_ap();
        }
    }

    void loop() {
        if (_portal) {
            _dns.processNextRequest();
            return;
        }

        // Watchdog: detect drop/reconnect and maintain mDNS + LED state.
        bool now_up = (WiFi.status() == WL_CONNECTED);
        if (now_up && !_connected) {
            _connected = true;
            debug_log::write(debug_log::INFO, SRC,
                "reconnected  ip=%s", WiFi.localIP().toString().c_str());
            MDNS.begin(MDNS_HOSTNAME);
            status_led::set(status_led::SOLID);
        } else if (!now_up && _connected) {
            _connected = false;
            debug_log::write(debug_log::WARN, SRC, "connection lost — waiting for auto-reconnect");
            status_led::set(status_led::BLINK_FAST);
        }
    }

    bool is_connected()   { return _connected; }
    bool in_portal_mode() { return _portal; }
}
