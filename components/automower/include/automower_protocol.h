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

    // Mower fault code (GetError, 4586/6) -> human text. Reverse-engineered
    // from the Husqvarna/Gardena Android app's shared error enum (see
    // research/notes/upstream-error-codes.py); numbering is shared across the
    // whole Automower/Gardena line so not every code applies to the GO 400.
    const char* mower_error_str(int32_t code);

    // Response frame's ResponseResult byte (byte[16]) -> human text.
    // 0=OK; nonzero means the mower understood the request but declined it.
    const char* result_str(uint8_t result);
}
