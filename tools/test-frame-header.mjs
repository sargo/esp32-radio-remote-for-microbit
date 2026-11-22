// Checks the logic of components/microbit_radio/microbit_frame.h without a C++
// compiler: mechanically transliterates the header to JavaScript (typed arrays
// stand in for pointers, casts become masks) and runs its self_test(), plus a
// decode of a live-built frame with the reference receiver.
import { readFileSync } from "node:fs";
import { nrfReceive, parseMakeCode, toBits, whiteningSequence, xorBits } from "./microbit-radio-encoder.mjs";

let src = readFileSync(new URL("../components/microbit_radio/microbit_frame.h", import.meta.url), "utf8");
src = src.slice(src.indexOf("namespace frame {") + "namespace frame {".length, src.indexOf("}  // namespace frame"));

const ARR = "(?:bits|inner|out|pdu|p|adv_bits)";
src = src
  .replace(/\/\/.*$/gm, "")
  .replace(/static constexpr \w+ (\w+) =/g, "const $1 =")
  .replace(/static const uint8_t expected\[4\]\[ADV_DATA_LEN\] = \{([\s\S]*?)\};/, (_, body) =>
    `const expected = [${body.replace(/\{/g, "Uint8Array.of(").replace(/\}/g, ")")}];`)
  .replace(/(?:const )?uint8_t (\w+)\[[^\]]+\] = \{0\};/g, "let $1 = new Uint8Array(64);")
  .replace(/(?:const )?uint8_t (\w+)\[[^\]]+\] = \{([\s\S]*?)\};/g, "let $1 = Uint8Array.of($2);")
  .replace(/uint8_t (\w+)\[([^\]]+)\];/g, "let $1 = new Uint8Array($2);")
  .replace(/inline \w+ (\w+)\(([^)]*)\)/g, (_, name, params) =>
    `function ${name}(${params.split(",").map((p) => p.trim().replace(/\[.*\]$/, "").split(/[\s*]+/).pop()).join(", ")})`)
  .replace(/static_cast<uint8_t>/g, "0xFF & ")
  .replace(/static_cast<uint16_t>/g, "0xFFFF & ")
  .replace(/static_cast<uint32_t>\(([^)]*)\)/g, "(($1) >>> 0)")
  .replace(/for \(size_t /g, "for (let ")
  .replace(/(?:const )?(?:size_t|uint8_t|uint16_t|int) (\w+) =/g, "let $1 =")
  .replace(/(?:size_t|uint8_t|uint16_t) (\w+);/g, "let $1;")
  .replace(new RegExp(`\\b(${ARR}) \\+ (\\w+(?: \\* \\d+)?)`, "g"), "$1.subarray($2)")
  .replace(/\(nbits \+ 7\) \/ 8/g, "((nbits + 7) >> 3)")
  .replace(/\[i \/ 8\]/g, "[i >> 3]");

const prelude = `
const memset = (d, v, n) => { for (let i = 0; i < n; i++) d[i] = v; };
const memcpy = (d, s, n) => { for (let i = 0; i < n; i++) d[i] = typeof s === "string" ? s.charCodeAt(i) : s[i]; };
const memcmp = (a, b, n) => { for (let i = 0; i < n; i++) if (a[i] !== b[i]) return 1; return 0; };`;
const mod = new Function(`${prelude}\n${src}\nreturn { self_test, make_value, make_string, build_adv_data };`)();

const st = mod.self_test();
console.log(`self_test() = ${st} ${st === 0 ? "(all vectors match)" : "(vector " + st + " FAILED)"}`);

// Round trip: header-built AdvData for sendValue("joy", -77) on channel 39, decoded by the reference receiver
const pkt = new Uint8Array(19), adv = new Uint8Array(31);
const n = mod.make_value(pkt, "joy", 3, -77, 12345, 0);
const ok = mod.build_adv_data(42, pkt, n, 39, adv);
const pdu = [0x42, 37, 1, 2, 3, 4, 5, 6, ...adv, 0, 0, 0];
const air = [...toBits([0xaa, 0xd6, 0xbe, 0x89, 0x8e]), ...xorBits(toBits(pdu), whiteningSequence(39, pdu.length * 8))];
const r = nrfReceive(air, 42);
console.log(`round trip: build=${ok} CRC=${r?.ok} ${r ? parseMakeCode(r.frame) : "no sync"}`);
const tooLong = mod.make_string(pkt, "0123456789", 10, 0, 0);
console.log(`string of 10 chars rejected: ${tooLong === 0}`);
process.exit(st === 0 && ok && r?.ok && tooLong === 0 ? 0 : 1);
