# BLE component — native NimBLE (ESP-IDF host) port of src/ble/ble_manager.cpp.
idf_component_register(
    SRCS
        "ble_manager.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        core
        automower
        bt
        nvs_flash
)
