# web component — esp_http_server-based HTTP/SSE/OTA server.
# ArduinoJson comes from the managed dependency declared in idf_component.yml,
# so it is NOT listed in REQUIRES here.
idf_component_register(
    SRCS "web_server.cpp"
    INCLUDE_DIRS "include"
    REQUIRES core net ble esp_http_server app_update esp_wifi esp_netif esp_timer mbedtls
)
