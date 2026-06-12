# mqtt component — esp-mqtt-based telemetry/command bridge + HA discovery.
# `mqtt` in REQUIRES is the esp-mqtt component from the managed dependency
# espressif/mqtt. ArduinoJson also comes from a managed dep (idf_component.yml),
# so it is NOT listed in REQUIRES here.
idf_component_register(
    SRCS "mqtt.cpp"
    INCLUDE_DIRS "include"
    REQUIRES core net ble espressif__mqtt esp_timer
)
