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

    const char* mower_error_str(int32_t code) {
        switch (code) {
            case 0:   return "No error";
            case 1:   return "Outside working area";
            case 2:   return "No loop signal";
            case 3:   return "Wrong loop signal";
            case 4:   return "Loop sensor problem, front";
            case 5:   return "Loop sensor problem, rear";
            case 6:   return "Loop sensor problem, left";
            case 7:   return "Loop sensor problem, right";
            case 8:   return "Wrong PIN code";
            case 9:   return "Trapped";
            case 10:  return "Upside down";
            case 11:  return "Low battery";
            case 12:  return "Empty battery";
            case 13:  return "No drive";
            case 14:  return "Mower lifted";
            case 15:  return "Lifted";
            case 16:  return "Stuck in charging station";
            case 17:  return "Charging station blocked";
            case 18:  return "Collision sensor problem, rear";
            case 19:  return "Collision sensor problem, front";
            case 20:  return "Wheel motor blocked, right";
            case 21:  return "Wheel motor blocked, left";
            case 22:  return "Wheel drive problem, right";
            case 23:  return "Wheel drive problem, left";
            case 24:  case 25:  return "Cutting system blocked";
            case 26:  return "Invalid sub-device combination";
            case 27:  return "Settings restored";
            case 28:  return "Memory circuit problem";
            case 29:  return "Slope too steep";
            case 30:  case 118: return "Charging system problem";
            case 31:  return "Stop button problem";
            case 32:  return "Tilt sensor problem";
            case 33:  return "Mower tilted";
            case 34:  return "Cutting stopped, slope too steep";
            case 35:  return "Wheel motor overloaded, right";
            case 36:  return "Wheel motor overloaded, left";
            case 37:  return "Charging current too high";
            case 38:  return "Electronic problem";
            case 39:  return "Cutting motor problem";
            case 40:  case 42:  return "Limited cutting height range";
            case 41:  return "Unexpected cutting height adjustment";
            case 43:  return "Cutting height problem, drive";
            case 44:  return "Cutting height problem, current";
            case 45:  return "Cutting height problem, direction";
            case 46:  return "Cutting height blocked";
            case 47:  return "Cutting height problem";
            case 48:  return "No response from charger";
            case 49:  return "Ultrasonic problem";
            case 50:  return "Guide 1 not found";
            case 51:  return "Guide 2 not found";
            case 52:  return "Guide 3 not found";
            case 53:  return "GPS navigation problem";
            case 54:  return "Weak GPS signal";
            case 55:  return "Difficulty finding home";
            case 56:  return "Guide calibration accomplished";
            case 57:  return "Guide calibration failed";
            case 58: case 59: case 60: case 61: case 62:
            case 63: case 64: case 65: case 68:
                      return "Temporary battery problem";
            case 66: case 67: case 127: return "Battery problem";
            case 69:  return "Alarm: mower switched off";
            case 70:  return "Alarm: mower stopped";
            case 71:  return "Alarm: mower lifted";
            case 72:  return "Alarm: mower tilted";
            case 73:  return "Alarm: mower in motion";
            case 74:  return "Alarm: outside geofence";
            case 75:  return "Connection changed";
            case 76:  return "Connection not changed";
            case 77:  return "Com board not available";
            case 78:  return "Slipped, mower has slipped";
            case 79:  return "Invalid battery combination";
            case 80:  return "Cutting system imbalance";
            case 81:  return "Safety function faulty";
            case 82:  return "Wheel motor blocked, rear right";
            case 83:  return "Wheel motor blocked, rear left";
            case 84:  return "Wheel drive problem, rear right";
            case 85:  return "Wheel drive problem, rear left";
            case 86:  return "Wheel motor overloaded, rear right";
            case 87:  return "Wheel motor overloaded, rear left";
            case 88:  return "Angular sensor problem";
            case 89:  return "Invalid system configuration";
            case 90:  return "No power in charging station";
            case 91:  return "Switch cord problem";
            case 92:  return "Work area not valid";
            case 93:  return "No accurate position from satellites";
            case 94:  return "Reference station communication problem";
            case 95:  return "Folding sensor activated";
            case 96:  return "Right brush motor overloaded";
            case 97:  return "Left brush motor overloaded";
            case 98:  return "Ultrasonic sensor 1 defect";
            case 99:  return "Ultrasonic sensor 2 defect";
            case 100: return "Ultrasonic sensor 3 defect";
            case 101: return "Ultrasonic sensor 4 defect";
            case 102: return "Cutting drive motor 1 defect";
            case 103: return "Cutting drive motor 2 defect";
            case 104: return "Cutting drive motor 3 defect";
            case 105: return "Lift sensor defect";
            case 106: return "Collision sensor defect";
            case 107: return "Docking sensor defect";
            case 108: return "Folding cutting deck sensor defect";
            case 109: return "Loop sensor defect";
            case 110: return "Collision sensor error";
            case 111: return "No confirmed position";
            case 112: return "Cutting system major imbalance";
            case 113: return "Complex working area";
            case 114: return "Too high discharge current";
            case 115: return "Too high internal current";
            case 116: return "High charging power loss";
            case 117: return "High internal power loss";
            case 119: return "Zone generator problem";
            case 120: return "Internal voltage error";
            case 121: return "High internal temperature";
            case 122: return "CAN error";
            case 123: return "Destination not reachable";
            case 124: return "Destination blocked";
            case 125: return "Battery needs replacement";
            case 126: return "Battery near end of life";
            case 128: return "Multiple reference stations detected";
            case 129: return "Auxiliary cutting means blocked";
            case 130: return "Imbalanced auxiliary cutting disc detected";
            case 131: return "Lifted in link arm";
            case 132: return "EPOS accessory missing";
            case 133: return "Bluetooth communication with CS failed";
            case 134: return "Invalid SW configuration";
            case 135: return "Radar problem";
            case 136: return "Work area tampered";
            case 137: return "High temperature in cutting motor, right";
            case 138: return "High temperature in cutting motor, center";
            case 139: return "High temperature in cutting motor, left";
            case 141: return "Wheel brush motor problem";
            case 143: return "Accessory power problem";
            case 144: return "Boundary wire problem";
            case 701: case 703: case 704: case 705: case 715: case 716:
                      return "Connectivity problem";
            case 702: return "Connectivity settings restored";
            case 706: return "Poor signal quality";
            case 707: return "SIM card requires PIN";
            case 708: case 710: case 711: case 712: return "SIM card locked";
            case 709: return "SIM card not found";
            case 713: case 714: return "Geofence problem";
            case 717: return "SMS could not be sent";
            case 724: return "Communication circuit board software must be updated";
            default:  return code < 0 ? "Unknown" : "Unknown error";
        }
    }

    const char* result_str(uint8_t result) {
        switch (result) {
            case 0:  return "OK";
            case 1:  return "UNKNOWN_ERROR";
            case 2:  return "INVALID_VALUE";
            case 3:  return "OUT_OF_RANGE";
            case 4:  return "NOT_AVAILABLE";
            case 5:  return "NOT_ALLOWED";
            case 6:  return "INVALID_GROUP";
            case 7:  return "INVALID_ID";
            case 8:  return "DEVICE_BUSY";
            case 9:  return "INVALID_PIN";
            case 10: return "MOWER_BLOCKED";
            default: return "UNKNOWN_RESULT";
        }
    }
}
