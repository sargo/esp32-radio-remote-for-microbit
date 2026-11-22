// Prints C++ self-test vectors for components/microbit_radio (time=0, serial=0).
import { mc, bleAdvData } from "./microbit-radio-encoder.mjs";
const cases = [
  ["number 42, group 1, ch 37", 1, 37, mc.number(42)],
  ['value "A"=1, group 1, ch 37', 1, 37, mc.value("A", 1)],
  ['string "a-d", group 7, ch 39', 7, 39, mc.string("a-d")],
  ["number -5, group 255, ch 38", 255, 38, mc.number(-5)],
];
for (const [label, group, ch, pkt] of cases) {
  const adv = bleAdvData(group, pkt, ch);
  console.log(`// ${label}\n{${adv.map((b) => "0x" + b.toString(16).padStart(2, "0")).join(", ")}},`);
}
