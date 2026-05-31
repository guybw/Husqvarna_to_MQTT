# Flymo / Husqvarna BLE → ESP32 → MQTT bridge

## Context

The user owns a **Flymo EasiLife GO 400** robotic mower (BLE advertisement: `EasiLife GO 400 (B0:10:A0:94:7C:9B)`, marketed as "Husqvarna Automower BLE"). They want to control it from Home Assistant.

Going via HA's native Bluetooth proxy stack is unreliable for this device. The plan is to **bypass HA's BT layer entirely** by putting a dedicated **ESP32** next to the mower that:

1. Talks the Automower BLE protocol directly to the mower.
2. Exposes a small **admin web UI** for setup, pairing, debug, and firmware updates.
3. (Later) bridges state and commands over **MQTT** so HA can consume it via MQTT discovery.

The BLE protocol is reverse-engineered by the **`alistair23/AutoMower-BLE`** Python project. The active maintained fork **`Marbanz/HusqvarnaAutoMower-BLE`** (v1.6.1, April 2026) is significantly more complete — more commands, a structured `protocol.json` listing every command/UUID, a 140+ entry error-code enum, and Flymo model-tuple support. Both are GPL-3.0-or-later. **Our firmware will use the Marbanz fork as the protocol source of truth** and will also be GPL-3.0.

This plan covers the **MVP firmware**: web UI, BLE pairing with PIN, command sending, debug log, OTA. MQTT is scoped but deferred — a clean hook will be left in place.

---

## CURRENT STATUS — 0.12.5-dev, 2026-05-31

This plan is the original design doc; below is where the project actually is.
The MVP, MQTT, and extensive sensor suite are complete and fully deployed.

**All milestones complete (M0–M8+).** Live on hardware since 2026-05-16.

### Feature Summary (0.12.5)
- **Core:** BLE handshake+PIN, secure bonding, WiFi+mDNS, web UI (embedded in firmware), OTA, SSE debug log
- **MQTT:** Full telemetry + command interface + Home Assistant MQTT discovery
- **Sensors:** Battery %, voltage, temperature, charging, state, activity, error, restriction, next-start,
  A-loop signals, pitch/roll/collision/lift, FrostSense, charging/cutting/running/searching times,
  power mode, charging cycles, blade usage, etc.
- **Commands:** Wake, Mow (override), Park, Pause, FrostSense toggle, LawnSense mode, Avoid Garage,
  daily schedule read/write
- **Behaviour:** Manual-wake model (DORMANT → Wake/MQTT → AUTHENTICATED). Passive auto-reconnect
  while mowing (15/30/60/120 s backoff); user Wake skips passive timeout.
- **Infrastructure:** Web flasher (GitHub Pages), pre-built `firmware.bin` release assets,
  GitHub Actions CI/build, complete HA automation templates (`docs/home-assistant.md`)

### Recent Major Work (0.11+)
- 0.12.5: Web flasher tool (GitHub Pages) + release publishing
- 0.12.2–0.12.3: Schedule read/write API (BLE task/calendar commands)
- 0.12.0–0.11.1: OTA→enable-BLE crash guard (post-OTA extra reboot)
- 0.10.0–0.10.1: Battery voltage, loop signals, FrostSense switch, auth toggle, command diagnostics
- 0.9.x: APK reverse-engineering (battery temp, pitch/roll, collision/lift, theft, statistics, power mode)
- 0.8.0: Charging status, error codes, restriction reason, next-start time (MQTT discovery)
- 0.7.x: MQTT telemetry publish + command subscribe + HA discovery
- 0.6.x: Manual-wake model (DORMANT + manual Wake)
- **Key fixes:** SSE/AsyncTCP thread-safety, post-OTA PANIC crash, platform/NimBLE pinning,
  RAM lean-down, passive auto-reconnect, FrostSense write path

---

### Headless-after-deployment constraint (important)

The ESP32 will be USB-tethered to the laptop only during initial bring-up. Once moved next to the mower it is **WiFi-only** — no serial, no easy physical access. Every recovery path the user might need after that point **must be reachable over WiFi**, or via a no-laptop physical action (e.g. button hold). This shapes several decisions below: web log is the primary debug surface (not Serial), OTA must be rock-solid (single bad flash = an unrecoverable trip up a ladder), mDNS is mandatory so the user can find the device without checking the router, and there's a button-driven NVS factory reset to recover from a bad WiFi-config change.

---

## Decisions (locked in)

| Decision | Choice |
|---|---|
| Board | **ESP32-WROOM-32** (classic ESP32 DevKit V1) |
| Framework | **Arduino**, built with **PlatformIO** |
| BLE stack | **NimBLE-Arduino** (smaller RAM footprint than the default Bluedroid stack — important when also running an async web server) |
| Web server | **ESPAsyncWebServer** + **AsyncTCP** |
| Filesystem | **LittleFS** (for serving HTML/CSS/JS from `/data`) |
| Settings store | **Preferences** (NVS) — for password hash, WiFi creds, paired mower MAC, PIN, MQTT config |
| WiFi onboarding | **Captive portal on first boot** (no creds saved → AP `FlymoBridge-setup`) |
| Auth | **HTTP Basic Auth**, default `admin / admin`, password changeable; stored as **SHA-256 hash** + per-device salt in NVS |
| OTA | **Web-form `.bin` upload** via `Update.h`, with **rollback-capable** partition layout. Also keep **ArduinoOTA** enabled in dev builds so we can push from PlatformIO over WiFi during early development. |
| Discovery | **mDNS** advertising as `flymo-bridge.local` — user must not need to hunt for the IP after the device is deployed |
| Recovery | **BOOT button (GPIO0) held >5 s during normal run** = wipe NVS WiFi creds + reboot → back to captive portal. Status LED flashes to confirm. |
| License | **GPL-3.0** |
| MQTT | Out of scope for MVP — port reserved in settings model and one stub file |
| Protocol source | **`Marbanz/HusqvarnaAutoMower-BLE`** fork; mirror its `protocol.json` and `error_codes.py` into `docs/` for in-tree reference |

---

## High-level architecture

```
┌──────────────────────────── ESP32-WROOM-32 ────────────────────────────┐
│                                                                          │
│  ┌──────────────┐   ┌──────────────┐   ┌────────────────────────────┐   │
│  │ WiFi manager │   │ Web server   │   │ BLE client (NimBLE)         │   │
│  │ + captive AP │   │ (Async)      │   │  - scan                     │   │
│  └──────┬───────┘   │  - /login    │   │  - connect                  │   │
│         │           │  - /setup    │   │  - 3-step handshake         │   │
│         │           │  - /pair     │◄─►│  - EnterOperatorPin(1234)   │   │
│         │           │  - /debug    │   │  - keep-alive 15 s          │   │
│         │           │  - /update   │   │  - command encode (CRC-8)   │   │
│         │           │  - /api/*    │   │  - response/notif decode    │   │
│         │           └──────┬───────┘   └──────────────┬──────────────┘   │
│         │                  │                          │                  │
│         │           ┌──────▼──────────────────────────▼──────────┐       │
│         │           │  In-RAM ring buffer:  BLE event log         │       │
│         │           │  (timestamped lines, drives /debug page)    │       │
│         │           └─────────────────────────────────────────────┘       │
│         │                                                                  │
│         └───────► NVS (Preferences):                                      │
│                     - wifi.ssid / wifi.psk                                 │
│                     - admin.user / admin.passhash / admin.salt            │
│                     - mower.mac / mower.pin / mower.name                  │
│                     - mqtt.* (reserved, unused in MVP)                    │
└──────────────────────────────────────────────────────────────────────────┘
                                  ▲
                                  │ Wi-Fi (LAN)
                                  │
                              your browser
```

`BLEManager`, `WebServer`, and `WiFiManager` all run on the Arduino `loop()` task; long-running work (BLE scan, command/response wait) is dispatched via FreeRTOS tasks so the web server stays responsive.

---

## Deployment lifecycle (USB → WiFi-only)

The development experience changes shape after the device is moved. The firmware must be designed around this from day one, not retrofitted.

**Phase A — Bench (USB-tethered to laptop):**
- Flash via USB: `pio run -t upload`.
- Serial monitor available: `pio device monitor -b 115200` shows boot/WiFi/BLE logs.
- LittleFS upload via USB: `pio run -t uploadfs`.
- Bring up M0–M5 here. Confirm scan, pair, command round-trip while you can still see serial.

**Phase B — Deployed (next to the mower, WiFi only):**
- **No serial.** All future debugging is via `/debug` (live SSE log) and `/api/system/info`.
- **OTA is the only way to update firmware:** `/update` page accepts `.bin` files from your laptop browser. We also keep ArduinoOTA enabled so `pio run -t upload --upload-port flymo-bridge.local` works.
- **Partition table is `default.csv` with two app slots + OTA-data**, so a bad flash boots the previous good image (esp32 bootloader rollback). This is the single most important safety net once the device is out of reach.
- **mDNS** publishes `flymo-bridge.local` on `_http._tcp` and `_arduino._tcp` so the user can reach the device without checking the router's DHCP table.
- **NVS-wipe escape hatch**: holding the on-board BOOT button (GPIO0) for >5 s during normal operation wipes WiFi creds and reboots into captive-portal mode. This is the recovery if a `/setup` WiFi change goes wrong.
- **Status LED** (on-board GPIO2 LED, used by most ESP32-WROOM-32 dev kits):
  - Off → boot / no WiFi
  - Slow blink (1 Hz) → AP / captive-portal mode
  - Fast blink (5 Hz) → connecting to WiFi or BLE
  - Solid → WiFi up, mower paired and reachable
  - Triple-blink burst → recent BLE error (clears after 10 s)

## Project layout

```
C:\Temp\Husqvarna_to_MQTT\
├── platformio.ini
├── CLAUDE.md               ← project context for future Claude sessions
├── LICENSE                 ← GPL-3.0
├── README.md               ← short, user-facing
├── partitions.csv          ← OTA-capable 2× app partition table
├── src/
│   ├── main.cpp            ← setup() / loop(), task orchestration
│   ├── config.h            ← compile-time defaults (AP SSID, default user, etc.)
│   ├── settings.{h,cpp}    ← Preferences/NVS wrapper
│   ├── wifi_manager.{h,cpp}← STA + captive portal fallback
│   ├── web_server.{h,cpp}  ← all HTTP routes, Basic Auth, OTA handler
│   ├── ble/
│   │   ├── ble_manager.{h,cpp}    ← scan / connect / disconnect / state
│   │   ├── automower_protocol.{h,cpp} ← packet framing, CRC-8, channel ID
│   │   ├── automower_commands.{h,cpp} ← EnterOperatorPin, GetState, Start, Pause, Park, GetBatteryLevel
│   │   └── crc8_maxim.{h,cpp}     ← 256-byte lookup table
│   ├── debug_log.{h,cpp}   ← ring buffer for the /debug page (primary debug surface after deployment)
│   ├── status_led.{h,cpp}  ← non-blocking LED state machine (boot / AP / WiFi / BLE / error)
│   ├── reset_button.{h,cpp}← GPIO0 long-press → NVS wipe + reboot
│   └── mqtt_stub.{h,cpp}   ← empty hook; future home of PubSubClient wiring
├── data/                   ← LittleFS image (uploaded with `pio run -t uploadfs`)
│   ├── login.html
│   ├── setup.html          ← change password, WiFi
│   ├── pair.html           ← scan, select, PIN entry
│   ├── debug.html          ← live log via /api/debug/stream (SSE)
│   ├── update.html         ← .bin upload form
│   └── style.css
└── docs/
    ├── protocol-notes.md          ← our cribbed summary of the packet format
    ├── upstream-protocol.json     ← verbatim copy from Marbanz fork (single source of truth)
    └── upstream-error-codes.py    ← verbatim copy; we generate a C++ enum from this
```

---

## BLE protocol (port from AutoMower-BLE)

Reference: `alistair23/AutoMower-BLE` (GPL-3.0). Key facts we need to implement in `src/ble/`:

**GATT identifiers (mower):**
- Service:   `98bd0001-0b0e-421a-84e5-ddbf75dc6de4`
- TX (write):`98bd0002-0b0e-421a-84e5-ddbf75dc6de4`
- RX (notify): `98bd0003-0b0e-421a-84e5-ddbf75dc6de4`

**Packet structure (request):**
```
byte 0       0x02                        start of frame
byte 1       0xFD                        type
bytes 2..3   payload length (LE)
bytes 4..7   channel ID (LE, random on connect)
byte 8       0x01                        linked flag
byte 9       CRC-8/MAXIM-DOW of bytes 0..8
byte 10      msg type   (0x00 req / 0x01 resp / 0x02 event)
byte 11      0xAF                        fixed
bytes 12..15 module:command IDs (major/minor as 2× uint16 LE)
bytes 16..17 inner payload length (LE)
bytes 18..N  payload (typed parameters: uint8/16/32, ascii, unix ts)
byte N+1     CRC-8 of payload
byte N+2     0x03                        end of frame
```

**Handshake on every new connection:**
1. Generate 32-bit channel ID, send channel-setup packet.
2. Send fixed handshake confirmation: `02 fd 0a 00 00 00 00 00 00 d0 08 01`.
3. Send `EnterOperatorPin` with the user-entered PIN (Flymo default **1234**).
4. Send empty keep-alive every **15 s** while connected; response timeout 10 s.

**MTU:** ATT MTU 23 → 20-byte BLE payload; framing longer than that requires chunking at **17 bytes**.

**Commands we implement in MVP** (expanded based on what the Marbanz fork exposes — these add real value for HA without much extra complexity):
| Name | IDs (major:minor) | Purpose |
|---|---|---|
| `EnterOperatorPin` | — | Auth |
| `GetState` | 4586:2 | Operation state |
| `GetActivity` | 4586:3 | What it's doing |
| `GetMode` | 4586:0 | Auto / manual / home etc. |
| `GetMowerError` | (see protocol.json) | Latest error code; decoded via `error_codes` table |
| `GetSerialNumber` | 4698:10 | Device ID — useful for MQTT topic + HA unique_id |
| `GetBatteryLevel` | 4106:20 | Battery % |
| `IsCharging` | 4106:21 | Charging? |
| `GetRemainingChargingTime` | 4106:22 | ETA to full |
| `GetNextStartTime` | 4658:1 | Next scheduled start (Unix ts) |
| `GetAllStatistics` | 4726:0 | Runtime / cycle counters |
| `StartTrigger` | 4586:4 | Start / resume |
| `Pause` | 4586:5 | Pause |
| `SetOverridePark` | 4658:4 | Park immediately |
| `SetOverrideMow` | 4658:3 | Mow for N hours |

**Source of truth:** mirror `protocol.json` and `error_codes.py` from `Marbanz/HusqvarnaAutoMower-BLE` into `docs/upstream-protocol.json` and `docs/upstream-error-codes.py` (license-compatible — both GPL-3.0). Generate the C++ command tables and error-code enum from those files where reasonable. Keep the mirrored files unmodified so future re-syncing is a diff.

**Flymo EasiLife GO 400 model code** is not explicitly listed in the fork's `models.py` (Flymo family = 30; known variants are GO 150/200/250/500). The GO 400 is almost certainly family 30, some variant. The pairing flow will read the model tuple from the device and log it; if it's an unknown variant, we'll treat it as the closest known Flymo and add it to a local override table in `src/ble/models.cpp`.

CRC-8 is `MAXIM-DOW` (polynomial 0x31). Port the 256-byte lookup table directly into `crc8_maxim.cpp`.

PIN-entry on the mower itself (button-mapped) is a one-time pairing step the **user does physically**: Power=1, Go/Schedule=2, Go=3, Park=4. We do not automate that; we only send the PIN over BLE after physical pairing. Default PIN `1234`.

---

## Web UI

All HTML/CSS lives in LittleFS (`/data`). Every route except `/login` requires HTTP Basic Auth against the stored hash+salt.

| Route | Method | What it does |
|---|---|---|
| `/login` | GET | Static login page |
| `/` | GET | Redirects to `/pair` if no mower paired, else `/debug` |
| `/setup` | GET / POST | Change admin password; show / change WiFi |
| `/pair` | GET | Page with "Scan" button, scan results, PIN field, "Pair" button |
| `/api/scan` | POST | Kick off BLE scan; returns scan id |
| `/api/scan/results` | GET | Latest scan results (JSON) |
| `/api/pair` | POST | `{mac, pin}` — connect + handshake + EnterOperatorPin; persist on success |
| `/api/unpair` | POST | Forget stored mower |
| `/api/command` | POST | `{cmd: "start"\|"pause"\|"park"\|"state"\|"battery"}` |
| `/debug` | GET | Live log page |
| `/api/debug/stream` | GET (SSE) | Server-Sent Events stream of ring-buffer lines |
| `/api/debug/log` | GET | Last N lines as JSON (fallback for browsers that hate SSE) |
| `/update` | GET / POST | `.bin` upload form → `Update.write()` → reboot. Re-prompts for Basic Auth before accepting upload. |
| `/api/system/info` | GET | Firmware version, build date, free heap, uptime, WiFi RSSI, mower paired Y/N, last BLE event time — small JSON so the user can sanity-check the device from a phone without reaching `/debug` |
| `/api/system/reboot` | POST | Soft reboot (recovery from BLE stack lockups, sometimes needed) |
| `/api/system/factory-reset` | POST | Wipes WiFi + admin + mower NVS. Same end-state as 5 s BOOT-hold. Requires re-typed password as a confirm. |

The **debug ring buffer** is a fixed-size circular array of `(millis, level, source, message)`. Every meaningful BLE event (scan found, connect, disconnect, sent packet hex, received packet hex, CRC fail, timeout, PIN-OK, PIN-FAIL, keep-alive) is appended. The SSE endpoint streams new lines as they're added.

---

## CLAUDE.md (to be created)

A pragmatic project guide for any future Claude session (here or in VS Code). It will include:

- One-paragraph project summary (BLE→ESP32→MQTT bridge for Flymo EasiLife).
- The locked-in decisions table from this plan.
- Build/flash commands:
  - `pio run` — compile
  - `pio run -t upload` — flash firmware
  - `pio run -t uploadfs` — flash LittleFS image (run after editing `data/`)
  - `pio device monitor -b 115200` — serial log
  - Web OTA: visit `http://<esp-ip>/update` and upload `.pio/build/esp32dev/firmware.bin`
- Coding conventions: GPL-3.0 header on every `.cpp`/`.h`; no dynamic allocation in BLE callbacks; log everything BLE-related via `debug_log::write(...)`.
- Pointer to `docs/protocol-notes.md` for packet format details.
- "Out of scope for MVP" section: MQTT, TLS, multi-mower, scheduling — so future sessions don't accidentally start them.
- A note that **AutoMower-BLE is GPL-3.0** and that any code resembling the protocol logic should attribute it.

---

## Implementation milestones

Each is a sensible commit point — the user can flash and sanity-check at each.

**Bench phase (USB available):**
- **M0 — Skeleton:** ✅ DONE
- **M1 — WiFi + captive portal + mDNS:** ✅ DONE
- **M2 — Web server + auth + factory reset:** ✅ DONE (Basic Auth disabled pending login-loop fix)
- **M3 — OTA (web `.bin` upload + ArduinoOTA):** ✅ DONE — both web OTA and ArduinoOTA working
- **M4 — BLE scan:** ✅ DONE — scan UI with auto-refresh, mower identified at `b0:10:a0:94:7c:9b`
- **M5 — BLE connect + handshake + PIN:** ✅ DONE (2026-05-16)
  - 3-phase handshake fully working: channel setup → handshake confirm → EnterOperatorPin
  - PIN=1234 confirmed correct; state → AUTHENTICATED achieved reliably
  - Current firmware at completion: 0.4.5-dev

- **M6 — Status polling + auto-reconnect:** ✅ DONE (2026-05-16)
  - notify_cb frame detection fixed (byte[11]==0xAF → len+4, else len+2)
  - decode_response fixed (result at [16], payload at [19], min 21 bytes)
  - GetState / GetActivity / GetBatteryLevel polled immediately after AUTHENTICATED
  - Live-confirmed values: state=2 (STOPPED), activity=0 (NONE), battery=100%
  - Keepalive (4674/2) fires every 15s; write failure forces reconnect
  - Exponential backoff reconnect (5s → 10s → 20s … cap 60s)
  - Channel ID persisted in NVS (`mower.chid`) — reused on reconnect so mower
    recognises same client and accepts CCCD write (subscribe with-response)
  - subscribe(with-response) used as readiness indicator: no-response → fast-fail
  - Phase 1 retries up to 3× within same BLE connection (2s gap between attempts)
  - Phase 1 race condition fixed: checks isConnected() + n>0 after rx_wait returns
  - Current firmware: 0.5.1-dev

- **M7 — Debug page:** ✅ DONE (built in M2/M3 — SSE debug log with ring buffer, level filter)

> **Deployment gate:** Before moving the ESP32 off the laptop, verify on the bench: factory-reset via BOOT-hold works, OTA upload + rollback works, mDNS resolves, `/debug` shows live BLE traffic. If any of these are flaky, fix them before the device goes out of reach.

**Deployed phase (WiFi only):**
- **M8 — MQTT:** NEXT — Wire PubSubClient. Publish state/battery/activity/error/charging to `flymo/<serial>/...`. Subscribe to `flymo/<serial>/cmd`. Ship HA MQTT-discovery config payloads.

- **M9 — UI redesign (after stable connection confirmed):**
  - Separate BLE scan and BLE pairing into distinct modes
  - "Scan Now" button → scan-only mode, no connection attempts, shows nearby devices
  - Same button becomes "Stop Scan" → exits scan mode, begins connecting to selected device
  - Auto-reconnect to saved mower continues in background (unaffected by scan mode)
  - Paired device panel: show currently paired mower (MAC, name, channel ID, last seen)
  - **Delete/forget button** per paired device — clears NVS mac/pin/chid, disconnects
  - Allows re-pairing to a different mower without factory reset
  - (Future) support for multiple saved mowers if hardware ever warrants it

## Protocol discoveries (from live captures + Alistair23 source)

### Frame format — both sides use len = total - 4
Alistair23 line 331: `length = data[2] + 4` — client AND mower use the same convention.

### Response layout differs from request
Response has extra `result` byte at [16]; payload starts at byte[19] (not [18] like requests).
Byte[16] = ResponseResult (0=OK, 9=INVALID_PIN, etc.)

### notify_cb must classify frames by byte[11]
- Standard frames: byte[11] == 0xAF → total = len_field + 4
- Handshake/link frames: byte[11] != 0xAF → total = len_field + 2
- Must wait for ≥12 bytes before classifying

### Channel ID must be persisted and reused (CRITICAL)
The mower keeps its channel session open after BLE disconnect — it never times out.
On reconnect, the client MUST send the same channel_id it used previously.
Sending a different channel_id causes the mower to reject the CCCD write (subscribe no-response → phase 1 always fails).
Fix: store channel_id in NVS (`mower.chid`); clear it only when user explicitly re-pairs.

### Subscribe mode = readiness indicator (confirmed from live logs)
- `subscribe(with-response)` → mower ACKed CCCD write → protocol stack ready → phase 1 succeeds
- `subscribe(no-response)` → mower returned ATT error → not ready → phase 1 always fails
- Firmware fast-fails on no-response mode instead of wasting 15 s on doomed retries

### Mower BLE behaviour
- Mower needs 2–4 connection attempts before responding to channel setup (after power-on)
- 5 s settle delay must be AFTER subscribe (Alistair23: `asyncio.sleep(5.0)` after `start_notify`)
- Must poll status immediately after AUTHENTICATED — mower may disconnect if idle
- onDisconnect releases rx_sem — phase 1 must check isConnected() + buf_len > 0 after rx_wait

### All command IDs confirmed in docs/AutoMower-BLE-main/automower_ble/protocol.json

---

## Critical files (where the real work happens)

- `platformio.ini` — board (`esp32dev`), framework (`arduino`), partition table (`default.csv` — two app slots, OTA-capable), library deps (`h2zero/NimBLE-Arduino`, `ESP32Async/ESPAsyncWebServer`, `ESP32Async/AsyncTCP`), and a `build_flags` entry that bakes in a firmware version string the user can see at `/api/system/info`.
- `src/ble/automower_protocol.cpp` — packet build/parse + CRC-8. Highest risk of subtle bugs; needs careful matching against Marbanz fork's `protocol.py` and `protocol.json`.
- `src/ble/automower_commands.cpp` — command encoders/decoders, generated/hand-translated from `docs/upstream-protocol.json`.
- `src/ble/ble_manager.cpp` — connection state machine + keep-alive + reconnect.
- `src/web_server.cpp` — routes, Basic Auth, OTA handler (`Update.h`), SSE stream.
- `src/wifi_manager.cpp` — captive portal logic + mDNS.
- `src/reset_button.cpp` — GPIO0 long-press detection (the single physical recovery path once deployed).
- `data/pair.html` — the page the user actually uses for pairing.
- `data/debug.html` — the page the user will live in once the device is deployed.

---

## Verification (end-to-end)

The order here is deliberately the same as the milestone order — and the **deployment-gate items (3, 4, 6) are verified before BLE work even starts**, because they're the user's only safety nets once the device is moved off the bench.

1. **Flash blank board.** Boot → AP `FlymoBridge-setup` should appear. Connect with phone, captive page opens, enter home WiFi creds → ESP reboots, joins network. Serial log shows obtained IP; `flymo-bridge.local` resolves from another machine.
2. **Auth.** Visit `http://flymo-bridge.local/`. Basic Auth prompt → `admin / admin` works. `/setup` changes the password; re-login succeeds with new password.
3. **Factory reset (BOOT-hold).** Hold GPIO0 for >5 s while running. LED confirms (rapid blink). Device reboots into AP `FlymoBridge-setup` — WiFi creds and admin password are wiped. This is the bench-verified physical-recovery path.
4. **OTA + rollback.** Modify firmware version string, `pio run` → upload `.bin` via `/update`. Device reboots, new version visible at `/api/system/info`. Then **deliberately flash a broken build** (e.g. a panic in `setup()`); confirm the bootloader rolls back to the previous good image and the device comes back up.
5. **mDNS.** From a fresh device on the same WiFi, `ping flymo-bridge.local` resolves.
6. **`/debug` page works as a no-serial debug surface.** Trigger every interesting boot/WiFi event and confirm the SSE stream shows it. From here on, *do not rely on serial output to validate behavior*.
7. **`/pair` page.** Click Scan → `EasiLife GO 400 (B0:10:A0:94:7C:9B)` shows up. Physically pair the mower (Flymo button sequence using default mapping Power=1, Go/Schedule=2, Go=3, Park=4). Enter PIN `1234` and click Pair → debug log shows handshake bytes and `PIN-OK`. Mower MAC + model tuple persisted (visible after refresh).
8. **Commands.** Click `GetBatteryLevel` → response decodes to a sensible percentage in the debug log. `GetSerialNumber` returns a real serial. `Pause`, then `StartTrigger` → mower physically responds. `SetOverridePark` parks it.
9. **WiFi loss recovery.** Toggle router off/on → ESP reconnects without reboot; mower BLE link re-establishes using stored MAC + PIN.
10. **BLE loss recovery.** Power-cycle the mower → ESP retries with exponential backoff, eventually reconnects without manual intervention; `/debug` shows the reconnect sequence.
11. **Deployment dry run.** Unplug USB, run the device from a USB power brick on the bench for an hour. Confirm `/debug` and `/update` still work via WiFi only. Only then move it to the mower.

---

## Out of scope (deferred, do not start without asking)

- ~~MQTT publishing / subscribing and HA MQTT-discovery payloads.~~ **DONE (M8).**
- TLS / HTTPS on the admin UI (LAN-only assumption).
- Multiple mowers.
- Mower scheduling / mapping / GPS.
- Mobile-app-style UI polish.
- Auto-update from a release server.

---

## Open items the user should decide later

- Whether `flymo-bridge.local` is the right name (could parameterise via `/setup`).
- Whether `/update` should require re-entering the password (extra Basic Auth challenge) or rely on the existing session. Default: require re-prompt, since this is the post-deployment escape hatch.
- Topic naming convention when MQTT lands (`flymo/<serial>/state` planned; serial is preferred over MAC because it's stable + user-recognisable + already returned by `GetSerialNumber`).
- Whether the deployed device should run a small inline reverse proxy / static page on `flymo-bridge.local/help` with the BOOT-button recovery instructions, for the inevitable "I forgot how to factory-reset it" moment.

---

## Recommended next step

~~(Original: build the M0 skeleton.)~~ — The project is built and deployed.
Remaining work, in priority order:

1. Decompile the Flymo APK (jadx) to recover the `GetBatteryTemperature`
   command ID, then add it as a sensor (same pattern as the other sensors).
2. `GetAllStatistics` (4726/0) → run/cut/charge-time sensors.
3. Diagnose the intermittent post-flash boot failure using the new
   `reset reason:` boot log (BROWNOUT ⇒ USB power; PANIC ⇒ code).
4. Map RestrictionReason codes to text once real-world values are observed.

---

## LIVING NOTES & OUTSTANDING (kept current — last update 2026-05-17, fw 0.9.2-dev)

This file is the project's single source of truth for notes/decisions/TODOs.
Update it whenever changes are made (alongside CHANGELOG.md + version bump).

### Done since original plan
- 0.9.0: APK decompiled (androguard; scripts in `docs/`, IDs in
  `docs/protocol-notes.md`). Added battery temp (4106:9), power mode (4674:1),
  GetAllStatistics (4726:0), theft (4736:21). Battery temp/power/stats
  confirmed live with correct scaling.
- 0.9.1: poll stability fix — optional sensors short-timeout +
  disable-on-fail + every-5th-tick (0.9.0 starved WiFi/MQTT/SSE).
- 0.9.2: corrected collision=4166:8, lift=4476:6, pitch=4958:0, roll=4958:1
  (the app's real commands); removed dead 20:4 mainboard-temp/upsidedown.
- 0.9.3: UI fixes (Maintenance→Update tab, scan-state, drop Mower refresh).
- 0.9.4: FrostSense step 1 — RestrictionReason→text + Frost-hold binary
  sensor (restriction==5). Live-confirmed: pitch/roll/collision/lift now
  answer; batt_temp raw °C correct.
- 0.9.5: removed theft (4736:21 unsupported on GO 400); added unpub_cfg so
  dropped sensors clear their retained HA discovery (board_temp/upsidedown/
  theft ghost entities). **Rule: removing a sensor must unpub_cfg it.**
- 0.9.6: FrostSense step 2 — read-only GetAllSettings 5370:8 (avail/enabled).
  Live: frost_avail/enabled = true → **GO 400 uses module 5370**.
- 0.9.7: fixed BLE background scan running in maintenance mode (begin()
  skipped so target not loaded → loop() scanned). Added `_started` guard on
  all BLE entry points.
- 0.10.0: battery voltage (4106:1) + loop signals (4462:13/14); FrostSense
  control switch (SetEnabled 5370:2 — first setting-write); WiFi SSID
  prefill + masked password (blank=keep); HTTP auth enable/disable toggle
  (default OFF, guard on serve_index + all POSTs, BOOT-hold = recovery).
- Recovered full RestrictedReason table: 0 NONE, 1 PARK_OVERRIDE,
  2 WEEK_SCHEDULE, 3 SENSOR, 4 DAILY_LIMIT, **5 FROST_SENSOR**,
  6 COMPLETED_MISSIONS.
- `docs/home-assistant.md`: monitoring automations + the monitor-vs-sleep model.

### Key findings
- **No board/ambient temperature exists** in the protocol. Whole Flymo app
  reads only battery temp (4106:9 / G3 5508:12). `mowertemp` only lived in
  20:4 which the GO 400 doesn't answer. Battery temp is the only thermal
  signal over BLE; ambient must come from HA weather.
- FrostSense uses the mower's own internal ambient sensor (NOT battery); that
  value/threshold is not exposed. But frost-active IS observable:
  state=RESTRICTED + RestrictionReason==5 (FROST_SENSOR), which we already
  poll (4658:0).

### UI feedback (user, 2026-05-17) — TODO
- [done 0.9.3] Maintenance Mode → move from Status tab to Update tab.
- [done 0.9.3] Status > Bluetooth Devices: shows "Scanning…" when the user
  didn't click Scan — only show that when user-initiated.
- [done 0.9.3] Mower Control: remove the redundant Refresh button
  (tab already auto-refreshes every 5 s).
- [done 0.10.0, BENCH-VERIFY] Setup: enable/disable password-auth toggle.
  Implemented: `settings::auth_enabled` (default OFF), `/api/auth`,
  `guard()` validating Basic vs salted SHA-256 on serve_index + all POSTs;
  GET telemetry open (LAN). BOOT-hold NVS wipe = lockout recovery.
  **MUST bench-verify before relying on it:** enable on the bench, confirm
  login works AND BOOT-hold recovers, before deploying headless. The old
  "login-loop" came from auth on every asset incl. SSE — guard now skips
  GET/SSE, so that path should be clear, but it is unproven on hardware.

### Live validation (0.10.0 payload, 2026-05-17)
- batt_voltage 20.35 V (docked/charged) — ÷1000 scaling **confirmed**.
- loop_strength 100% matches app — GO 400 uses the **4462 (LoopSystem)**
  variant, not 4834. loop_a/f/guide answer; absolute values not comparable
  to old screenshots (position- and time-dependent).
- **Roll offset CONFIRMED**: roll pinned 20–21° across all payloads while
  pitch tracks level (−2…1); mower hasn't moved and is at most on a *very
  slight* slope (≪21°). So ~20° is a fixed uncalibrated mounting offset the
  app zeroes out and we don't. TODO: capture raw roll at a known-level
  reference and store/subtract a calibration offset (expose a "calibrate
  roll" action); pitch needs no offset. Not yet built.
- **FrostSense switch CONFIRMED working (0.10.1, 2026-05-17).** TX
  `02 fd 11 00 …fa 14 02 00 01 00…` (major 5370, minor 2, bool payload),
  RX result byte 00 = OK, "cmd: frost_on OK" first try. The earlier
  0.10.0 failure was the mower asleep, NOT a framing bug. FrostSense
  steps 1–3 all done & validated. **Operational rule: this mower naps
  extremely fast at state=7/activity=5 — send any write within a few
  seconds of AUTHENTICATED (toggle promptly after Wake).** 0.10.1's
  retry + short timeout + TX/RX logging stays as the robust path for all
  future write commands.
- Post-flash boots have been clean (reset reason: POWERON/SW) — the
  OTA→enable-BLE crash is a *specific* trigger, not every boot.
- 0.11.1: OTA→enable-BLE crash = **PANIC confirmed** (NimBLE init on first
  post-OTA boot; clean on next). Mitigation: post-OTA flag forces one extra
  clean reboot before BLE init. Bench-verify the double-reboot. Root-cause
  backtrace not obtainable headless — this is a known-pattern workaround.
- 0.11.0: **pitch/roll scaling bug fixed** (raw is deci-degrees, app ÷10;
  the "roll=20" was 2.0°, not a calibration offset — roll-calibration
  TODO retired). Added **Avoid Garage** (4692:3/10) HA switch and
  **LawnSense** (Autotimer 4460:5/7/8) HA select Off/Low/Medium/High.
  Total times now "Xd Yh Zm" and Next start a readable datetime in HA.

### Fix list / new requests (user, 2026-05-17)
- **BUG (repro): crash on the reboot after OTA-then-enable-BLE.** Steps:
  do a firmware OTA, then disable maintenance mode (re-enable Bluetooth) →
  the device reliably crashes on that reboot. Likely the same root as the
  tracked "intermittent post-flash boot failure" but now with a solid
  repro: first BLE-enabled boot straight after a fresh flash. Capture the
  `reset reason:` boot log on that crash boot (PANIC ⇒ code; BROWNOUT ⇒
  USB power). High priority — it's on the deploy-critical OTA path.
- **HA Bluetooth on/off control** (debugging convenience). Maps to the
  existing maintenance-mode flag, but note: maintenance is *boot-time*
  (BLE only (re)starts via `begin()` at boot), so an MQTT switch toggling
  it would still **require a reboot per toggle** (heavy, and currently the
  exact path that crashes — see bug above). A true runtime BLE on/off
  needs `ble_manager` to support stop/teardown + start without reboot
  (NimBLE deinit/init) — bigger change. Decide: ship the reboot-based
  switch (fix the crash first) vs. implement runtime enable/disable.

### Sensor organization / HA Diagnostics refactor
- **Planned**: Move diagnostic-type sensors to HA's Diagnostics entity category:
  - A-loop signal strength (4462:13/14) → Diagnostics
  - Pitch/roll/collision/lift → Diagnostics (mower orientation/tilt sensors)
  - Battery voltage (4106:1) → Diagnostics (raw voltage, ÷1000 scaling)
  - Avoid garage / LawnSense status → Diagnostics
  - Guide-wire / perimeter signals → Diagnostics
- **Keep as primary sensors**: Battery %, charging, FrostSense, state, activity
- **Rationale**: Reduces main dashboard clutter; diagnostics remain visible for
  troubleshooting but grouped under HA's entity_category: "diagnostic"
- This is an MQTT discovery config change only — no firmware logic changes.

### Other outstanding (see CLAUDE.md + TodoWrite)
- Setup > WiFi: prefill current SSID; masked password placeholder, blank=keep
  (needs backend to expose stored SSID + "password set" bool — not returned
  by any API today).
- FrostSense: (1) decode RestrictionReason→text + frost binary_sensor
  **[done 0.9.4]**; (2) read-only status GetAllSettings 5370:8
  **[done 0.9.6]** — live result tells us if 5370 is the GO 400's module
  variant; if null, the mower uses 5412 (try GetEnabled 5412:1);
  (3) SetEnabled 5412:2/5370:2 + HA switch (first write-setting path —
  deliberate, after step 2 confirms the variant).
- **Framework upgrade: Arduino → ESP-IDF** (high-stability improvement for
  production use; substantial refactor, lower priority than feature work).
- **Schedule support**: Daily start/stop times per day of week (requires MQTT
  command + BLE write path for scheduling API — high feature value for users).
- Hardware swap to the better-antenna ESP (blocked on identifying its chip:
  classic ESP32 = straight reflash; S3/C3 = new PlatformIO env). MQTT topic
  id is the mower MAC so HA entities/automations survive the swap.
- Pre-existing: intermittent post-flash boot failure (reset-reason logging
  added); flash at ~94% (rollback margin shrinking).
