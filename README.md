# Husqvarna to MQTT — ESP32 Bridge

A Bluetooth bridge that connects your Husqvarna/Flymo mower to Home Assistant via MQTT.

## ⚡ Flash Firmware

**[🔗 Click here to flash your ESP32](https://guybw.github.io/Husqvarna_to_MQTT/)**

**Requirements:**
- ESP32-WROOM-32 microcontroller (~$10)
- USB cable
- Chrome, Edge, or Brave browser (Firefox has limited support)

**Steps:**
1. Connect your ESP32 via USB
2. Visit the flasher link above
3. Click **Connect** and select your COM port
4. Click **Flash Firmware** and wait ~1 minute

## ⚙️ Configure

After flashing, the ESP32 broadcasts a WiFi access point:

1. **Connect** to the AP (default: `Flymo-XXXXXX`)
2. **Open** `http://192.168.4.1` in your browser
3. **Enter:**
   - WiFi SSID and password
   - MQTT broker address
   - Mower PIN (usually `1234`)
   - Mower MAC address (found via scan)

## ✨ Features

- 🔋 Real-time battery, state, and activity status
- 📱 Start/pause/park commands via MQTT
- 🏠 Home Assistant MQTT Discovery
- 🔐 Secure BLE pairing with bonding
- 🌐 Web UI for configuration and debugging
- ♻️ OTA firmware updates via web UI
- ⚡ Energy-efficient (manual wake mode)

## 📋 What You'll Need

- **ESP32-WROOM-32** with BLE support
- **WiFi network** for the bridge
- **MQTT broker** (Mosquitto, Home Assistant, etc.)
- **Husqvarna/Flymo mower** with Bluetooth

## 🐛 Troubleshooting

**Can't connect to COM port?**
- Install [CP210x drivers](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers) (Windows)
- Try a different USB cable (some are power-only)

**Flash fails?**
- Hold **BOOT** button while clicking **Flash** in the browser
- Try a different USB port
- Check browser permissions for USB access

**Can't connect to mower?**
- Verify mower MAC address in web UI
- Try PIN `1234` (default)
- Power-cycle both devices

## 🚧 Roadmap & Known Issues

### High Priority
- [ ] **Framework upgrade: Arduino → ESP-IDF** — Migrate from Arduino framework to native ESP-IDF for better stability, control, and performance
- [ ] **Schedule support** — Add configurable daily start/stop times for automated mowing schedules
- [ ] **Boot reliability** — Investigate and fix intermittent "won't start after flash" issue (currently requires power-cycle in rare cases)

### Medium Priority
- [ ] **Battery temperature sensor** — `GetBatteryTemperature` command not supported on Flymo EasiLife GO 400 (may be available on other mower models)

### Nice to Have
- [ ] Advanced scheduling (weather-based, grass length-based)
- [ ] Home Assistant automation templates
- [ ] Enhanced logging and diagnostics

---

**Version:** 0.12.4-dev | **License:** GPL-3.0-or-later | [Source Code](https://github.com/guybw/Husqvarna_to_MQTT)
