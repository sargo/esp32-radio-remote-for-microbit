// Prototype: build micro:bit (CODAL/DAL MicroBitRadio + MakeCode radio) frames
// for transmission from a non-Nordic radio, and verify them with a simulated
// nRF receiver written along a different code path.
//
// micro:bit PHY (from codal-microbit-v2 MicroBitRadio.cpp):
//   MODE=Nrf_1Mbit, FREQUENCY=band (2400+band MHz, default 7), BASE0=0x75626974,
//   PREFIX0=group, BALEN=4 (5-byte address), PCNF0: LFLEN=8, PCNF1: WHITEEN=1,
//   little endian, MAXLEN=32, CRC 16-bit init 0xFFFF poly 0x11021 (incl. address),
//   DATAWHITEIV=0x18.
// Frame after address: [len][version=1][group][protocol=1 datagram][payload...]

const BASE = [0x74, 0x69, 0x62, 0x75]; // 0x75626974 sent LSByte first ("tibu")
const PREAMBLE = 0xaa; // first address bit is 0 -> 0xAA (LSB first: 0,1,0,1...)

// ---------- bit helpers (on-air order: each byte LSbit first) ----------
const toBits = (bytes) => bytes.flatMap((b) => [0, 1, 2, 3, 4, 5, 6, 7].map((i) => (b >> i) & 1));
const toBytes = (bits) => {
  const out = [];
  for (let i = 0; i < bits.length; i += 8) {
    let b = 0;
    for (let j = 0; j < 8 && i + j < bits.length; j++) b |= bits[i + j] << j;
    out.push(b);
  }
  return out;
};

// ---------- whitening, written from the Nordic/BLE spec ----------
// LFSR g(D)=D^7+D^4+1, positions 0..6, position 0 = 1, positions 1..6 = IV bits 5..0.
function whiteningSequence(iv, nbits) {
  let r = [1, (iv >> 5) & 1, (iv >> 4) & 1, (iv >> 3) & 1, (iv >> 2) & 1, (iv >> 1) & 1, iv & 1];
  const seq = [];
  for (let i = 0; i < nbits; i++) {
    const out = r[6];
    seq.push(out);
    r = [out, r[0], r[1], r[2], r[3] ^ out, r[4], r[5]];
  }
  return seq;
}
const xorBits = (a, b) => a.map((v, i) => v ^ b[i]);

// ---------- CRC-16/CCITT bit-serial over on-air bits, output MSB first ----------
function crcBits(bits) {
  let crc = 0xffff;
  for (const b of bits) {
    const fb = ((crc >> 15) & 1) ^ b;
    crc = (crc << 1) & 0xffff;
    if (fb) crc ^= 0x1021;
  }
  return [...Array(16)].map((_, i) => (crc >> (15 - i)) & 1);
}

// ---------- MakeCode radio packet (pxt-common-packages libs/radio/radio.ts) ----------
// | 0 type | 1..4 time (int32 LE) | 5..8 serial (int32 LE) | 9.. payload
const int32le = (v) => [v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >>> 24) & 0xff];
const ascii = (s) => [...s].map((c) => c.charCodeAt(0));
const mc = {
  number: (n, time = 0) => [0, ...int32le(time), ...int32le(0), ...int32le(n)],
  value: (name, n, time = 0) => [1, ...int32le(time), ...int32le(0), ...int32le(n), name.length, ...ascii(name)],
  string: (s, time = 0) => [2, ...int32le(time), ...int32le(0), s.length, ...ascii(s)],
};

// On-air bits starting at the address: address + whitened(len..payload + CRC)
function microbitFrameBits(group, mcPacket) {
  const pdu = [3 + mcPacket.length, 1, group, 1, ...mcPacket];
  if (pdu[0] > 32) throw new Error(`frame too long: ${pdu[0]} > MAXLEN 32`);
  const addrBits = toBits([...BASE, group]);
  const pduBits = toBits(pdu);
  const body = [...pduBits, ...crcBits([...addrBits, ...pduBits])];
  return [...addrBits, ...xorBits(body, whiteningSequence(0x18, body.length))];
}

// (A) Raw packet for ESPeranto transmit_packet() (BLE TX test mode, no BLE whitening,
//     set_swapping(false) because micro:bit is LSbit first like BLE).
const esperantoPacket = (group, mcPacket) => [PREAMBLE, ...toBytes(microbitFrameBits(group, mcPacket))];

// (B) Legacy BLE advertising "packet-in-packet": 31-byte AdvData for
//     esp_ble_gap_config_adv_data_raw(), advertised on a single channel.
//     AdvData starts 8 bytes (header 2 + AdvA 6) into the BLE-whitened PDU.
const BLE_CH_FREQ = { 37: 2402, 38: 2426, 39: 2480 };
function bleAdvData(group, mcPacket, channel = 37) {
  const inner = [PREAMBLE, ...toBytes(microbitFrameBits(group, mcPacket))];
  if (inner.length > 31) throw new Error(`inner frame ${inner.length} B > 31 B AdvData`);
  const padded = [...inner, ...Array(31 - inner.length).fill(0)];
  const w = whiteningSequence(channel, 8 * (8 + 31)).slice(64);
  return toBytes(xorBits(toBits(padded), w));
}

// ---------- simulated nRF receiver (independent code path) ----------
// Port of ESPeranto's dewhiten_ble() (byte-wise, bit-swapped LFSR) and its
// byte-wise MSB-first crc16(); BLE whitening "channel" 0x18 == DATAWHITEIV 0x18.
const swap = (v) => {
  v = ((v & 0xf0) >> 4) | ((v & 0x0f) << 4);
  v = ((v & 0xcc) >> 2) | ((v & 0x33) << 2);
  return ((v & 0xaa) >> 1) | ((v & 0x55) << 1);
};
function dewhitenBle(data, channel) {
  let lfsr = swap(channel) | 2;
  return data.map((d) => {
    let c = swap(d);
    for (let j = 7; j >= 0; j--) {
      if (lfsr & 0x80) { lfsr ^= 0x11; c ^= 1 << j; }
      lfsr = (lfsr << 1) & 0xff;
    }
    return swap(c);
  });
}
function crc16Bytes(bytes) {
  let crc = 0xffff;
  for (const b of bytes) {
    crc ^= b << 8;
    for (let i = 0; i < 8; i++) crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
  }
  return crc;
}
function nrfReceive(airBits, group) {
  const addr = toBits([...BASE, group]);
  for (let i = 0; i + addr.length <= airBits.length; i++) {
    if (!addr.every((b, k) => airBits[i + k] === b)) continue;
    const rest = toBytes(airBits.slice(i + addr.length));
    const len = dewhitenBle(rest.slice(0, 1), 0x18)[0];
    if (len > 32) continue;
    const d = dewhitenBle(rest.slice(0, 1 + len + 2), 0x18);
    // CRC over on-air bit order == byte-wise MSB-first CRC over bit-swapped bytes
    const calc = crc16Bytes([...BASE, group, ...d.slice(0, 1 + len)].map(swap));
    const rx = (swap(d[1 + len]) << 8) | swap(d[2 + len]);
    return { ok: calc === rx, frame: d.slice(0, 1 + len) };
  }
  return null;
}
function parseMakeCode(frame) {
  const [len, version, grp, proto, ...p] = frame;
  const i32 = (o) => p[o] | (p[o + 1] << 8) | (p[o + 2] << 16) | (p[o + 3] << 24);
  const str = (o) => String.fromCharCode(...p.slice(o + 1, o + 1 + p[o]));
  const head = `v${version} group=${grp} proto=${proto}`;
  if (p[0] === 0) return `${head} -> onReceivedNumber(${i32(9)})`;
  if (p[0] === 1) return `${head} -> onReceivedValue("${str(13)}", ${i32(9)})`;
  if (p[0] === 2) return `${head} -> onReceivedString("${str(9)}")`;
  return `${head} -> type ${p[0]}`;
}

export { mc, bleAdvData, esperantoPacket, nrfReceive, parseMakeCode, toBits, whiteningSequence, xorBits };

// ---------- demo / self-test ----------
// run the demo only when executed directly (not when imported by gen-vectors.mjs)
if (process.argv[1]?.endsWith("microbit-radio-encoder.mjs")) main();
function main() {
const hex = (a) => a.map((b) => b.toString(16).padStart(2, "0")).join(" ");
const GROUP = 1;
const cases = [
  ["sendNumber(42)", mc.number(42)],
  ['sendValue("A", 1)', mc.value("A", 1)],
  ['sendString("a-d")', mc.string("a-d")],
  ['sendString("joyX=1023")', mc.string("joyX=1023")],
];
let allOk = true;
for (const [label, pkt] of cases) {
  console.log(`\n=== ${label}, group ${GROUP}  (MakeCode packet ${pkt.length} B, radio frame len=${3 + pkt.length})`);

  // (A) raw packet; on air it sits inside a BLE test packet (preamble+AA+2B header before it)
  const a = esperantoPacket(GROUP, pkt);
  const airA = toBits([0xaa, 0x29, 0x41, 0x76, 0x71, 0x00, 0xff, ...a, 0, 0, 0]);
  const rA = nrfReceive(airA, GROUP);
  console.log(`A ESPeranto raw (${a.length} B): ${hex(a)}`);
  console.log(`  nRF sim @2407 MHz: CRC ${rA?.ok ? "OK" : "FAIL"}  ${rA ? parseMakeCode(rA.frame) : "no sync"}`);
  allOk &&= !!rA?.ok;

  // (B) advertising packet-in-packet on channel 37 (2402 MHz -> micro:bit band 2)
  try {
    const adv = bleAdvData(GROUP, pkt, 37);
    const header = [0x42, 6 + 31]; // ADV_NONCONN_IND, TxAdd=random
    const advA = [0x11, 0x22, 0x33, 0x44, 0x55, 0xc6];
    const pdu = [...header, ...advA, ...adv, 0x12, 0x34, 0x56 /* CRC24, irrelevant */];
    const pduBits = toBits(pdu);
    const airB = [...toBits([0xaa, 0xd6, 0xbe, 0x89, 0x8e]), ...xorBits(pduBits, whiteningSequence(37, pduBits.length))];
    const rB = nrfReceive(airB, GROUP);
    console.log(`B BLE AdvData ch37 (31 B): ${hex(adv)}`);
    console.log(`  nRF sim @2402 MHz: CRC ${rB?.ok ? "OK" : "FAIL"}  ${rB ? parseMakeCode(rB.frame) : "no sync"}`);
    allOk &&= !!rB?.ok;
  } catch (e) {
    console.log(`B BLE AdvData: does not fit (${e.message})`);
  }

  // negative test: wrong group must not sync / must not pass
  const rWrong = nrfReceive(airA, GROUP + 1);
  if (rWrong?.ok) { allOk = false; console.log("  !! wrong group accepted"); }
}
// cross-check the two whitening implementations directly
const probe = [...Array(40)].map((_, i) => (i * 37 + 11) & 0xff);
const same = hex(dewhitenBle(probe, 0x18)) === hex(toBytes(xorBits(toBits(probe), whiteningSequence(0x18, 320))));
console.log(`\nWhitening: spec LFSR (IV 0x18) == ESPeranto dewhiten_ble(ch 24): ${same}`);
console.log(allOk && same ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
}
