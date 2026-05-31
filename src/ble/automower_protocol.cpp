// SPDX-License-Identifier: GPL-3.0-or-later
#include "automower_protocol.h"
#include "crc8_maxim.h"
#include <string.h>

// Frame layout (all multi-byte fields little-endian):
//
//   [02][FD][len_L][len_H]          client sends len=total-4; mower echoes len=total-2
//   [ch_id_0..3]                    32-bit channel ID
//   [linked=01][hdr_crc]            linked flag + CRC8(bytes 1..8)
//   [00][AF]                        msg_type=request, fixed 0xAF
//   [major_L][major_H]
//   [minor_L][minor_H]
//   [plen_L][plen_H]                inner payload length
//   [payload...]
//   [payload_crc][03]               CRC8(payload), end marker
//
// Minimum frame (no payload): 20 bytes.

namespace automower {
    const char* SERVICE_UUID          = "98bd0001-0b0e-421a-84e5-ddbf75dc6de4";
    const char* CHAR_WRITE_UUID       = "98bd0002-0b0e-421a-84e5-ddbf75dc6de4";
    const char* CHAR_NOTIFY_UUID      = "98bd0003-0b0e-421a-84e5-ddbf75dc6de4";
    const char* CHAR_DEVICE_TYPE_UUID = "98bd0004-0b0e-421a-84e5-ddbf75dc6de4";

    static uint32_t _channel_id = 0;

    void set_channel_id(uint32_t id) { _channel_id = id; }

    size_t encode_request(uint8_t* out, size_t out_max,
                          uint16_t major_id, uint16_t minor_id,
                          const uint8_t* payload, size_t payload_len) {
        size_t total = 20 + payload_len;
        if (total > out_max) return 0;

        uint16_t len = (uint16_t)(total - 4);

        out[0] = 0x02;
        out[1] = 0xFD;
        out[2] = (uint8_t)(len & 0xFF);
        out[3] = (uint8_t)(len >> 8);
        out[4] = (uint8_t)(_channel_id & 0xFF);
        out[5] = (uint8_t)((_channel_id >> 8) & 0xFF);
        out[6] = (uint8_t)((_channel_id >> 16) & 0xFF);
        out[7] = (uint8_t)(_channel_id >> 24);
        out[8] = 0x01; // linked
        out[9] = crc8::maxim(out + 1, 8); // hdr_crc covers bytes[1..8], not byte[0]

        out[10] = 0x00; // msg_type = request
        out[11] = 0xAF;
        out[12] = (uint8_t)(major_id & 0xFF);
        out[13] = (uint8_t)(major_id >> 8);
        out[14] = (uint8_t)(minor_id & 0xFF);
        out[15] = (uint8_t)(minor_id >> 8);
        out[16] = (uint8_t)(payload_len & 0xFF);
        out[17] = (uint8_t)(payload_len >> 8);

        if (payload && payload_len > 0)
            memcpy(out + 18, payload, payload_len);

        uint8_t pcrc = crc8::maxim(out + 1, total - 3);
        out[18 + payload_len] = pcrc;
        out[19 + payload_len] = 0x03;

        return total;
    }

    bool decode_response(const uint8_t* in, size_t in_len,
                         uint16_t* out_major_id, uint16_t* out_minor_id,
                         const uint8_t** out_payload, size_t* out_payload_len) {
        // Mower response layout (confirmed from Alistair23 protocol.py):
        //   [02][FD][len_L][len_H]  len = total - 4  (same as client)
        //   [ch_id 4B][0x01][hdr_crc]
        //   [0x01][0xAF]            msg_type=response
        //   [major 2B][minor 2B]
        //   [result]                byte[16] — ResponseResult (0=OK, 9=INVALID_PIN…)
        //   [rlen_L][rlen_H]        bytes[17..18] — response payload length
        //   [payload...]            starts at byte[19]
        //   [pcrc][0x03]
        // Minimum (0 payload): 21 bytes; len_field = 17.
        if (in_len < 21) return false;
        if (in[0] != 0x02 || in[1] != 0xFD) return false;
        if (in[in_len - 1] != 0x03) return false;

        uint16_t len_field = (uint16_t)(in[2] | ((uint16_t)in[3] << 8));
        if ((size_t)(len_field + 4) != in_len) return false;

        if (in[9] != crc8::maxim(in + 1, 8)) return false;

        uint16_t major  = (uint16_t)(in[12] | ((uint16_t)in[13] << 8));
        uint16_t minor  = (uint16_t)(in[14] | ((uint16_t)in[15] << 8));
        // in[16] = result (0=OK); callers can check if needed
        uint16_t rlen   = (uint16_t)(in[17] | ((uint16_t)in[18] << 8));

        if (in_len != (size_t)(21 + rlen)) return false;

        *out_major_id    = major;
        *out_minor_id    = minor;
        *out_payload     = in + 19;
        *out_payload_len = rlen;
        return true;
    }
}
