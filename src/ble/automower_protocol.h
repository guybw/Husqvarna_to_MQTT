// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stddef.h>
#include <stdint.h>

// Low-level packet framing for the Automower BLE protocol.
// Reference: Marbanz/HusqvarnaAutoMower-BLE -- husqvarna_automower_ble/protocol.py
// and docs/upstream-protocol.json.
//
// Frame layout (see docs/protocol-notes.md for the full byte map).
//
// Implemented in M5.

namespace automower {
    // GATT identifiers exposed by the mower.
    extern const char* SERVICE_UUID;
    extern const char* CHAR_WRITE_UUID;
    extern const char* CHAR_NOTIFY_UUID;
    extern const char* CHAR_DEVICE_TYPE_UUID; // read-only; reading it wakes the app layer

    // 32-bit channel ID assigned per BLE connection. Set during the handshake.
    void set_channel_id(uint32_t id);

    // Encode a request frame into `out`. Returns bytes written (0 on overflow).
    size_t encode_request(uint8_t* out, size_t out_max,
                          uint16_t major_id, uint16_t minor_id,
                          const uint8_t* payload, size_t payload_len);

    // Decode a response frame from `in`. Returns true on success and fills
    // the out params. payload may be null inside the frame; the caller owns
    // nothing -- pointer references into `in`.
    bool decode_response(const uint8_t* in, size_t in_len,
                         uint16_t* out_major_id, uint16_t* out_minor_id,
                         const uint8_t** out_payload, size_t* out_payload_len);
}
