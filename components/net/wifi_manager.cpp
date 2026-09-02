// SPDX-License-Identifier: GPL-3.0-or-later
#include "wifi_manager.h"
#include "config.h"
#include "settings.h"
#include "debug_log.h"
#include "status_led.h"

#include <cstring>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "mdns.h"
#include "lwip/sockets.h"

#define SRC "wifi"

// Native ESP-IDF port notes
// -------------------------
// The Arduino version blocked in begin() polling WiFi.status(); here connection
// progress arrives as events (WIFI_EVENT / IP_EVENT) and begin() waits on an
// event group for GOT_IP up to WIFI_CONNECT_TIMEOUT_MS. Auto-reconnect is the
// disconnect handler re-calling esp_wifi_connect() (suppressed in portal mode).
// The captive portal's DNS (Arduino DNSServer) is a small UDP task that answers
// every A query with the softAP IP; loop() is therefore a no-op (state is
// event-driven) but kept for API compatibility with the rest of the firmware.

namespace wifi_manager {

    static bool               _connected = false;
    static bool               _portal    = false;
    static bool               _mdns_up   = false;
    static EventGroupHandle_t _eg        = nullptr;
    static esp_netif_t*       _sta_netif = nullptr;
    static esp_netif_t*       _ap_netif  = nullptr;
    static TaskHandle_t       _dns_task  = nullptr;

    static const int BIT_GOT_IP = BIT0;

    static void start_mdns() {
        if (_mdns_up) return;
        if (mdns_init() != ESP_OK) {
            debug_log::write(debug_log::WARN, SRC, "mDNS init failed");
            return;
        }
        mdns_hostname_set(MDNS_HOSTNAME);
        mdns_instance_name_set(FW_NAME);
        mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
        _mdns_up = true;
        debug_log::write(debug_log::INFO, SRC, "mDNS ready: %s.local", MDNS_HOSTNAME);
    }

    // Minimal captive DNS: reply to every query with an A record = softAP IP.
    static void dns_task(void*) {
        int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s < 0) { vTaskDelete(nullptr); return; }

        struct sockaddr_in sa = {};
        sa.sin_family      = AF_INET;
        sa.sin_port        = htons(53);
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
            close(s);
            vTaskDelete(nullptr);
            return;
        }

        esp_netif_ip_info_t ipinfo = {};
        if (_ap_netif) esp_netif_get_ip_info(_ap_netif, &ipinfo);
        uint32_t ap_ip = ipinfo.ip.addr; // already network byte order

        uint8_t buf[512];
        while (true) {
            struct sockaddr_in from; socklen_t fl = sizeof(from);
            int n = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr*)&from, &fl);
            if (n < 12) continue;           // smaller than a DNS header

            buf[2] = 0x81; buf[3] = 0x80;   // flags: response, recursion available
            buf[6] = 0x00; buf[7] = 0x01;   // ANCOUNT = 1 (QDCOUNT left as-is)

            uint8_t ans[16]; int a = 0;
            ans[a++] = 0xC0; ans[a++] = 0x0C;                 // name -> offset 12
            ans[a++] = 0x00; ans[a++] = 0x01;                 // type  A
            ans[a++] = 0x00; ans[a++] = 0x01;                 // class IN
            ans[a++] = 0x00; ans[a++] = 0x00; ans[a++] = 0x00; ans[a++] = 0x3C; // TTL 60
            ans[a++] = 0x00; ans[a++] = 0x04;                 // RDLENGTH 4
            memcpy(&ans[a], &ap_ip, 4); a += 4;               // RDATA = AP IP

            if (n + a <= (int)sizeof(buf)) {
                memcpy(buf + n, ans, a);
                sendto(s, buf, n + a, 0, (struct sockaddr*)&from, fl);
            }
        }
    }

    static void start_ap() {
        _portal   = true;
        _ap_netif = esp_netif_create_default_wifi_ap();
        // Run AP+STA: the softAP serves the captive portal while an active STA
        // interface lets the setup page scan for networks (esp_wifi_scan_start
        // needs a started station). The STA does NOT auto-connect in portal mode
        // — see the WIFI_EVENT_STA_START handler. Create the STA netif if the
        // STA-first path didn't already (no-saved-SSID boot calls start_ap directly).
        if (!_sta_netif) _sta_netif = esp_netif_create_default_wifi_sta();

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        wifi_config_t wc = {};
        strncpy((char*)wc.ap.ssid, AP_SSID, sizeof(wc.ap.ssid) - 1);
        wc.ap.ssid_len      = strlen(AP_SSID);
        wc.ap.max_connection = 4;
        wc.ap.authmode      = WIFI_AUTH_OPEN;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
        ESP_ERROR_CHECK(esp_wifi_start());

        esp_netif_ip_info_t ip = {};
        esp_netif_get_ip_info(_ap_netif, &ip);
        debug_log::write(debug_log::INFO, SRC,
            "AP mode  ssid=%s  ip=" IPSTR, AP_SSID, IP2STR(&ip.ip));
        status_led::set(status_led::BLINK_SLOW);

        xTaskCreate(dns_task, "captdns", 3072, nullptr, 5, &_dns_task);
    }

    static void on_event(void*, esp_event_base_t base, int32_t id, void* data) {
        if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
            // In portal mode the STA exists only so the setup page can scan; it
            // has no credentials, so don't try to connect (would just spam
            // STA_DISCONNECTED). In STA mode, connect to the saved network.
            if (!_portal) esp_wifi_connect();
        } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
            if (_connected) {
                _connected = false;
                debug_log::write(debug_log::WARN, SRC,
                    "connection lost — waiting for auto-reconnect");
                status_led::set(status_led::BLINK_FAST);
            }
            if (!_portal) esp_wifi_connect();   // keep retrying in STA mode
        } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t* e = (ip_event_got_ip_t*)data;
            _connected = true;
            debug_log::write(debug_log::INFO, SRC, "STA connected  ip=" IPSTR,
                IP2STR(&e->ip_info.ip));
            start_mdns();
            status_led::set(status_led::SOLID);
            xEventGroupSetBits(_eg, BIT_GOT_IP);
        }
    }

    void begin() {
        _eg = xEventGroupCreate();

        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            on_event, nullptr, nullptr);
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            on_event, nullptr, nullptr);

        std::string ssid = settings::get_wifi_ssid();
        if (ssid.empty()) {
            debug_log::write(debug_log::INFO, SRC, "no saved SSID — starting AP");
            start_ap();
            return;
        }

        debug_log::write(debug_log::INFO, SRC, "STA connecting ssid=%s", ssid.c_str());
        _sta_netif = esp_netif_create_default_wifi_sta();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

        wifi_config_t wc = {};
        std::string psk = settings::get_wifi_psk();
        strncpy((char*)wc.sta.ssid,     ssid.c_str(), sizeof(wc.sta.ssid) - 1);
        strncpy((char*)wc.sta.password, psk.c_str(),  sizeof(wc.sta.password) - 1);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
        status_led::set(status_led::BLINK_FAST);
        ESP_ERROR_CHECK(esp_wifi_start());

        EventBits_t bits = xEventGroupWaitBits(
            _eg, BIT_GOT_IP, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

        if (!(bits & BIT_GOT_IP)) {
            // We only get here with a saved SSID (the no-SSID case started the AP
            // above and returned). Do NOT fall back to the captive portal: that
            // latched _portal=true with no path back to STA, so a device powered
            // up before its WiFi was available (router still booting after a
            // power cut) stayed stuck in AP mode until a manual power-cycle.
            // Instead keep the STA running — the WIFI_EVENT_STA_DISCONNECTED
            // handler keeps calling esp_wifi_connect(), so it joins the moment
            // the network reappears. The portal is still reachable by wiping
            // credentials (hold the reset button RESET_HOLD_MS) or a no-SSID boot.
            debug_log::write(debug_log::WARN, SRC,
                "STA not up after %u ms — staying in STA, retrying until it appears",
                WIFI_CONNECT_TIMEOUT_MS);
        }
    }

    void loop() {
        // State is maintained by the event handler; nothing to poll.
    }

    bool is_connected()   { return _connected; }
    bool in_portal_mode() { return _portal; }
}
