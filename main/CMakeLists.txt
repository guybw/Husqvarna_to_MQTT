# main component — wires together all ported modules.
idf_component_register(
    SRCS
        "app_main.cpp"
    INCLUDE_DIRS
        "."
    REQUIRES
        nvs_flash
        esp_timer
        core
        automower
        net
        ble
        web
        mqtt_bridge
)
