#pragma once
// Encoder for micro:bit radio frames (CODAL/DAL MicroBitRadio + MakeCode "radio"
// packets) hidden inside a legacy BLE advertisement ("packet-in-packet").
//
// micro:bit PHY: Nordic Nrf_1Mbit, BASE0=0x75626974 ("ubit"), PREFIX0=group,
// 8-bit LENGTH, little endian, CRC-16/CCITT (init 0xFFFF, address included),
// whitening IV 0x18. Frame after the address: [len][1][group][1][MakeCode packet].
//
// The 31-byte result is handed to HCI LE Set Advertising Data. The BLE controller
// whitens it for the advertising channel, so it is pre-whitened here; on air the
// AdvData field then reads: 0xAA, "tibu", group, micro:bit-whitened frame, CRC.
//
// Reference implementation: tools/microbit-radio-encoder.mjs; test vectors: tools/gen-vectors.mjs.
// No ESPHome dependencies, so it can be unit-tested on a PC.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace esphome {
namespace microbit_radio {
namespace frame {

static constexpr size_t ADV_DATA_LEN = 31;
static constexpr size_t INNER_OVERHEAD = 1 + 5 + 4 + 2;  // preamble, address, len+header, CRC
static constexpr size_t MAX_MAKECODE_LEN = ADV_DATA_LEN - INNER_OVERHEAD;  // 19 bytes
static constexpr size_t MAKECODE_PREFIX_LEN = 9;  // type + time + serial
static constexpr size_t MAX_VALUE_NAME_LEN = MAX_MAKECODE_LEN - MAKECODE_PREFIX_LEN - 4 - 1;  // 5
static constexpr size_t MAX_STRING_LEN = MAX_MAKECODE_LEN - MAKECODE_PREFIX_LEN - 1;          // 9

static constexpr uint8_t PACKET_TYPE_NUMBER = 0;
static constexpr uint8_t PACKET_TYPE_VALUE = 1;
static constexpr uint8_t PACKET_TYPE_STRING = 2;

// Bits are kept one per byte, in on-air order (each byte LSbit first).
inline void bytes_to_bits(const uint8_t *src, size_t n, uint8_t *bits) {
  for (size_t i = 0; i < n; i++)
    for (size_t b = 0; b < 8; b++)
      bits[i * 8 + b] = (src[i] >> b) & 1;
}

inline void bits_to_bytes(const uint8_t *bits, size_t nbits, uint8_t *out) {
  memset(out, 0, (nbits + 7) / 8);
  for (size_t i = 0; i < nbits; i++)
    out[i / 8] |= static_cast<uint8_t>(bits[i] << (i % 8));
}

// BLE / Nordic whitening, g(D) = D^7 + D^4 + 1. Position 0 = 1, positions 1..6 =
// iv bits 5..0 (iv = BLE channel index, or micro:bit DATAWHITEIV 0x18).
// The first `skip` bits of the sequence are discarded.
inline void whiten(uint8_t *bits, size_t nbits, uint8_t iv, size_t skip) {
  uint8_t r[7] = {1,
                  static_cast<uint8_t>((iv >> 5) & 1),
                  static_cast<uint8_t>((iv >> 4) & 1),
                  static_cast<uint8_t>((iv >> 3) & 1),
                  static_cast<uint8_t>((iv >> 2) & 1),
                  static_cast<uint8_t>((iv >> 1) & 1),
                  static_cast<uint8_t>(iv & 1)};
  for (size_t i = 0; i < skip + nbits; i++) {
    const uint8_t out = r[6];
    r[6] = r[5];
    r[5] = r[4];
    r[4] = r[3] ^ out;
    r[3] = r[2];
    r[2] = r[1];
    r[1] = r[0];
    r[0] = out;
    if (i >= skip)
      bits[i - skip] ^= out;
  }
}

// CRC-16/CCITT, bit-serial over on-air bits.
inline uint16_t crc16_bits(const uint8_t *bits, size_t nbits) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < nbits; i++) {
    const uint8_t fb = static_cast<uint8_t>(((crc >> 15) & 1) ^ bits[i]);
    crc = static_cast<uint16_t>(crc << 1);
    if (fb)
      crc ^= 0x1021;
  }
  return crc;
}

inline void put_int32_le(uint8_t *p, uint32_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
  p[2] = (v >> 16) & 0xFF;
  p[3] = (v >> 24) & 0xFF;
}

// ---- MakeCode packets: | type | time (int32 LE) | serial (int32 LE) | payload |
// Each returns the packet length, or 0 if it does not fit into one advertisement.

inline size_t make_number(uint8_t *out, int32_t value, uint32_t time, uint32_t serial) {
  out[0] = PACKET_TYPE_NUMBER;
  put_int32_le(out + 1, time);
  put_int32_le(out + 5, serial);
  put_int32_le(out + 9, static_cast<uint32_t>(value));
  return 13;
}

inline size_t make_value(uint8_t *out, const char *name, size_t name_len, int32_t value, uint32_t time,
                         uint32_t serial) {
  if (name_len > MAX_VALUE_NAME_LEN)
    return 0;
  out[0] = PACKET_TYPE_VALUE;
  put_int32_le(out + 1, time);
  put_int32_le(out + 5, serial);
  put_int32_le(out + 9, static_cast<uint32_t>(value));
  out[13] = static_cast<uint8_t>(name_len);
  memcpy(out + 14, name, name_len);
  return 14 + name_len;
}

inline size_t make_string(uint8_t *out, const char *str, size_t len, uint32_t time, uint32_t serial) {
  if (len > MAX_STRING_LEN)
    return 0;
  out[0] = PACKET_TYPE_STRING;
  put_int32_le(out + 1, time);
  put_int32_le(out + 5, serial);
  out[9] = static_cast<uint8_t>(len);
  memcpy(out + 10, str, len);
  return 10 + len;
}

// ---- Wrap a MakeCode packet into 31 bytes of pre-whitened AdvData.
// ble_channel: 37, 38 or 39 (micro:bit band 2, 26 or 80). Returns false if too long.
inline bool build_adv_data(uint8_t group, const uint8_t *makecode, size_t makecode_len, uint8_t ble_channel,
                           uint8_t out[ADV_DATA_LEN]) {
  if (makecode_len > MAX_MAKECODE_LEN)
    return false;

  // Radio PDU: length, version, group, protocol (1 = datagram), MakeCode packet
  uint8_t pdu[4 + MAX_MAKECODE_LEN];
  const size_t pdu_len = 4 + makecode_len;
  pdu[0] = static_cast<uint8_t>(3 + makecode_len);
  pdu[1] = 1;
  pdu[2] = group;
  pdu[3] = 1;
  memcpy(pdu + 4, makecode, makecode_len);

  // On-air bits: address (40) + PDU + CRC (16)
  const uint8_t address[5] = {0x74, 0x69, 0x62, 0x75, group};  // BASE0 LSByte first, then PREFIX0
  uint8_t bits[(5 + 4 + MAX_MAKECODE_LEN + 2) * 8];
  bytes_to_bits(address, 5, bits);
  bytes_to_bits(pdu, pdu_len, bits + 40);
  const size_t crc_pos = 40 + pdu_len * 8;
  const uint16_t crc = crc16_bits(bits, crc_pos);
  for (size_t i = 0; i < 16; i++)
    bits[crc_pos + i] = (crc >> (15 - i)) & 1;  // CRC goes out MSbit first
  whiten(bits + 40, pdu_len * 8 + 16, 0x18, 0);  // micro:bit whitening covers PDU + CRC

  // Inner frame bytes: preamble + address + whitened PDU/CRC, zero padded to 31
  uint8_t inner[ADV_DATA_LEN] = {0};
  inner[0] = 0xAA;
  bits_to_bytes(bits, crc_pos + 16, inner + 1);

  // Pre-compensate BLE whitening; AdvData starts after header (2 B) + AdvA (6 B)
  uint8_t adv_bits[ADV_DATA_LEN * 8];
  bytes_to_bits(inner, ADV_DATA_LEN, adv_bits);
  whiten(adv_bits, ADV_DATA_LEN * 8, ble_channel, 8 * 8);
  bits_to_bytes(adv_bits, ADV_DATA_LEN * 8, out);
  return true;
}

// ---- Self-test against vectors from tools/gen-vectors.mjs (time = serial = 0).
// Returns the index of the first failing vector + 1, or 0 if all pass.
inline int self_test() {
  static const uint8_t expected[4][ADV_DATA_LEN] = {
      // number 42, group 1, ch 37
      {0xdf, 0x45, 0x78, 0x2a, 0xe3, 0x76, 0x70, 0xea, 0x63, 0x23, 0x90, 0x2c, 0xef, 0xf0, 0xc7, 0x8d,
       0xd2, 0x57, 0xa1, 0x17, 0xa7, 0x66, 0xb0, 0xf2, 0x43, 0xf4, 0x55, 0x68, 0xcf, 0xa9, 0x19},
      // value "A"=1, group 1, ch 37
      {0xdf, 0x45, 0x78, 0x2a, 0xe3, 0x76, 0x72, 0xea, 0x63, 0x23, 0x91, 0x2c, 0xef, 0xf0, 0xc7, 0x8d,
       0xd2, 0x57, 0xa1, 0x3c, 0xa7, 0x66, 0xb0, 0x74, 0x70, 0xf9, 0x5d, 0x68, 0xcf, 0xa9, 0x19},
      // string "a-d", group 7, ch 39
      {0x6b, 0xa2, 0xac, 0x26, 0x55, 0x5e, 0x56, 0xe8, 0xac, 0xd1, 0x9c, 0x53, 0x33, 0xd8, 0xba, 0x98,
       0x08, 0x24, 0xcb, 0x38, 0x9d, 0x5c, 0xc7, 0x7c, 0x09, 0x8d, 0xd2, 0x57, 0xa1, 0x3d, 0xa7},
      // number -5, group 255, ch 38
      {0xb1, 0xd1, 0xc6, 0x20, 0x0e, 0xb1, 0x45, 0x69, 0x30, 0xa8, 0x19, 0x6c, 0x5d, 0x4c, 0x04, 0x92,
       0xe5, 0x1d, 0xfe, 0x43, 0xae, 0x05, 0xd5, 0x61, 0x45, 0x31, 0x11, 0x48, 0x96, 0x77, 0xf8},
  };
  uint8_t pkt[MAX_MAKECODE_LEN];
  uint8_t adv[ADV_DATA_LEN];
  size_t n;

  n = make_number(pkt, 42, 0, 0);
  if (!build_adv_data(1, pkt, n, 37, adv) || memcmp(adv, expected[0], ADV_DATA_LEN) != 0)
    return 1;
  n = make_value(pkt, "A", 1, 1, 0, 0);
  if (!build_adv_data(1, pkt, n, 37, adv) || memcmp(adv, expected[1], ADV_DATA_LEN) != 0)
    return 2;
  n = make_string(pkt, "a-d", 3, 0, 0);
  if (!build_adv_data(7, pkt, n, 39, adv) || memcmp(adv, expected[2], ADV_DATA_LEN) != 0)
    return 3;
  n = make_number(pkt, -5, 0, 0);
  if (!build_adv_data(255, pkt, n, 38, adv) || memcmp(adv, expected[3], ADV_DATA_LEN) != 0)
    return 4;
  return 0;
}

}  // namespace frame
}  // namespace microbit_radio
}  // namespace esphome
