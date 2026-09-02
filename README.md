# Husqvarna to MQTT — ESP32 Bridge

Control and monitor your **Husqvarna Automower**, **Flymo EasiLife**, or **Gardena**
robotic lawn mower from **Home Assistant** — fully **local, with no cloud account**
and no *Husqvarna Connect* / *Automower Connect* subscription.

An **ESP32** (ESP32-WROOM-32) pairs with the mower over **Bluetooth Low Energy
(BLE)**, decodes the mower protocol on-device, and publishes live status and
controls to **MQTT** with **Home Assistant auto-discovery**. Built on **native
ESP-IDF**; this repo ships the readable application source plus a ready-to-flash
firmware binary — flash it from your browser and configure everything from the
on-device web UI.

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

1. **Connect** to the AP (default: `FlymoBridge-setup`)
2. **Open** `http://192.168.4.1` in your browser
3. **Enter:**
   - WiFi SSID and password
   - MQTT broker address
   - Mower PIN (usually `1234`)
   - Mower MAC address (found via scan)

## ✨ Features

- 🔋 Real-time battery, state, and activity status
- 📱 Start / pause / resume + three park modes (until next schedule · until further notice · timed) via MQTT & web UI
- ⚠️ Fault codes decoded to plain English, with a one-click "Clear error" once fixed
- 🏠 Home Assistant MQTT Discovery
- 🔐 Secure BLE pairing with bonding
- 🌐 Web UI for configuration and debugging
- ♻️ OTA firmware updates via web UI
- ⚡ Energy-efficient, sleep-respecting connection cadence

## 📋 What You'll Need

- **ESP32-WROOM-32** with BLE support
- **WiFi network** for the bridge
- **MQTT broker** (Mosquitto, Home Assistant, etc.)
- **Husqvarna/Flymo mower** with Bluetooth

## ✅ Compatibility

Works with **Husqvarna Group Bluetooth (BLE) robot mowers** that speak the
*AutoMower BLE* protocol — the mowers you pair with a **PIN in the manufacturer's
app**. These are sold under the **Flymo**, **Husqvarna**, **Gardena**, and
**McCulloch** brands.

- **Tested:** Flymo EasiLife GO 400, Flymo EasiLife 500.
- **Likely compatible:** other Flymo EasiLife / EasiLife GO models and Husqvarna
  Automower / Gardena / McCulloch BLE mowers using the same protocol. Behaviour can
  vary by model and firmware — reports (working or not) are welcome via **Issues**.
- **Not** for cloud/LTE-only Automower Connect models that have no local Bluetooth
  pairing.

> **Keywords / search terms:** Flymo EasiLife · Flymo EasiLife GO 400 / 500 ·
> Husqvarna Automower · Gardena robot mower · McCulloch ROB · Bluetooth (BLE)
> robotic lawn mower → **Home Assistant** over **MQTT** with an **ESP32** ·
> local / offline / **no-cloud** · no Husqvarna Connect or Automower Connect
> subscription · ESPHome / Bluetooth proxy alternative.

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

## 🗂️ Repository Layout

| Path | Purpose |
|------|---------|
| `components/` | Application source (BLE, MQTT, web server, settings, protocol) |
| `main/` | Firmware entry point |
| `data/` | Web UI assets (single-page app) |
| `tools/` | Helper scripts |
| `docs/` | Browser flasher + the flashable firmware binaries |

The prebuilt firmware lives in `docs/` and powers the one-click web flasher above.

## 💡 Why This Exists

Home Assistant can talk to BLE devices over an [ESP32 Bluetooth proxy](https://esphome.io/components/bluetooth_proxy.html),
but in practice that path is unreliable for Husqvarna/Flymo mowers — the
connection drops, bonding doesn't survive reboots, and the mower's sleep cadence
fights the proxy's polling. This project takes a different approach: a **dedicated
ESP32 that owns the BLE pairing and bond itself**, decodes the mower's protocol
on-device, and exposes everything to Home Assistant over plain MQTT (with auto
Discovery). One bonded device, one stable MQTT connection — no BLE proxy needed.

## 🎛️ What You Can View & Change

Everything below appears automatically in Home Assistant via MQTT Discovery
(grouped under one "Flymo Mower" device). Status and diagnostics are read from the
mower; controls write straight back to it over BLE.

### Status (read-only)
| Entity | What it shows |
|--------|---------------|
| Battery | Charge level (%) |
| Status | Off / Wait PIN / Stopped / Fatal error / Pending start / Paused / Mowing / Restricted / Error |
| Activity | None / Charging / Leaving / Mowing / Returning / Parked / Stopped in garden |
| Charging | Whether the mower is on the dock charging |
| Restriction reason | Why mowing is paused (Park override, Week schedule, Sensor, Daily limit, Frost, Missions complete) |
| Next start | Timestamp of the next scheduled run |
| Collision | Bump detected |
| Frost hold | Mowing suspended by the frost sensor |

### Controls (read/write)
| Control | Effect |
|---------|--------|
| Wake / Mow / Pause (buttons) | Send the corresponding command to the mower |
| Park — 3 modes (buttons) | Until next schedule · **until further notice** (holds home, ignores the schedule) · timed / custom duration — mirrors the official app's park menu |
| Resume (button) | Cancel a park/override and return to the weekly schedule |
| FrostSense (switch) | Enable/disable frost protection |
| Avoid garage (switch) | Tell the mower a garage/house module is installed |
| LawnSense (select) | Off / Low / Medium / High auto-timer sensitivity |
| Drive past wire (number) | Front overrun distance past the boundary wire, 0–50 cm (default 32) |
| Collision sensitivity (select) | Low / Medium / High bump responsiveness |

### Diagnostics (read-only, under HA's Diagnostic section)
Bridge connection · Charge time remaining · Error code · Battery temperature ·
Pitch & roll angle · Lifted · Power mode · FrostSense available/enabled · Battery
voltage · Loop signal strength (A-loop, F-loop, guide-wire) · Total running /
cutting / charging / searching time · Lifetime collisions · Charging cycles ·
Blade usage time · **Schedule** (summary + full task list as attributes).

### Device settings (on-device web UI)
| Setting | Notes |
|---------|-------|
| WiFi SSID / password | Network the bridge joins |
| MQTT host / port / user / password | Broker connection (empty host = MQTT off) |
| Mower MAC / PIN / name | Pairing details (PIN usually `1234`) |
| Admin user / password + HTTP auth | Optional login gate for the web UI |
| Maintenance mode | Keeps BLE off on boot for safe OTA flashing |
| Mow override duration | "Mow now" run length (default 3 h) |
| Idle re-check interval | How often a resting mower is woken for a status read (default 60 min) |
| Factory reset | Wipes all settings and reboots back to setup AP |

### Schedule editor

The web UI includes a **schedule editor** (under the Schedule section once logged
in). Click **Load schedule** to pull the mower's current weekly plan, edit the
per-day start times and durations in the table (up to 16 tasks, durations up to
1092 min), then **Save to mower** to write the whole schedule back over BLE. This
is the one piece of mower behaviour you can't easily set from the official app
once it's bonded to the bridge.

## 🚧 Known Issues

- **Schedule is web-UI only** — the schedule editor lives in the on-device web UI, not in MQTT/Home Assistant. View and edit the weekly plan from the bridge's web page; it isn't exposed as an HA control.

---

## 📚 Credits & Attribution

This project builds on the excellent work of the open-source community:

- **[alistair23/AutoMower-BLE](https://github.com/alistair23/AutoMower-BLE)** — Python BLE protocol implementation and reference
- **[Marbanz/HusqvarnaAutoMower-BLE](https://github.com/Marbanz/HusqvarnaAutoMower-BLE)** — Extended protocol documentation and command reference

---

**Version:** 0.22.2-dev | **Framework:** native ESP-IDF | **License:** GPL-3.0-or-later | [GitHub](https://github.com/guybw/Husqvarna_to_MQTT)
