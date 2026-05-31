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

## 🔐 How to Pair Your Mower

### Step 1: Put Mower in Pairing Mode

**Note:** Mower must be in the dock before pairing.

1. **Turn off** the mower by holding the **Power** button
2. **Turn it on** by pressing the **Power** button once
3. **Enter the PIN** using the mower's buttons (see PIN code below)
4. **Pairing mode is active** for 2–3 minutes

### Step 2: Understanding PIN Codes

Flymo mowers use button sequences to enter the PIN. The default PIN is **1234**, which translates to:

| Button | Code |
|--------|------|
| **Power** (On/Off) | **1** |
| **Go/Schedule** button | **2** |
| **Go** button | **3** |
| **Park** button | **4** |

**Example:** To enter PIN `1234`:
- Press Power button → releases (code 1)
- Press Go/Schedule button → releases (code 2)
- Press Go button → releases (code 3)
- Press Park button → releases (code 4)

**Note:** PIN mapping is specific to Flymo units. Other Husqvarna/Automower models may have different button layouts.

### Step 3: Pair via Web UI

1. Open the bridge's web UI at `http://192.168.4.1` (or your configured IP)
2. Go to the **Bluetooth Devices** tab
3. Click **Scan Now** to find your mower
4. Once found, select your mower and enter the PIN (e.g., `1234`)
5. Click **Pair**
6. The bridge will authenticate and save the bond for future connections

Bonding persists across reboots — you won't need to enter the PIN again unless the bond is explicitly cleared.

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
- [ ] **HA Diagnostics reorganization** — Move A-loop signals, pitch/roll/collision, and voltage sensors to Home Assistant's Diagnostics category (cleaner main dashboard)

### Nice to Have
- [ ] Advanced scheduling (weather-based, grass-height-based)
- [ ] Home Assistant automation templates and scripts
- [ ] Enhanced logging and remote diagnostics

---

## 📚 Credits & Attribution

This project builds on the excellent reverse-engineering work of the open-source community:

- **[alistair23/AutoMower-BLE](https://github.com/alistair23/AutoMower-BLE)** — Python BLE protocol implementation and reference
- **[Marbanz/HusqvarnaAutoMower-BLE](https://github.com/Marbanz/HusqvarnaAutoMower-BLE)** — Extended protocol documentation and command reference
- **[Husqvarna/Flymo](https://www.husqvarna.com/)** — Official mower hardware and firmware

BLE protocol reverse-engineered from official Flymo app (v6.12.0) and live device testing.

---

**Version:** 0.12.4-dev | **License:** GPL-3.0-or-later | [GitHub](https://github.com/guybw/Husqvarna_to_MQTT)
