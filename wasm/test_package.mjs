// test_package.mjs - checks the npm package (wasm/dist/npm) as a consumer
// would use it: ES module and CommonJS, the state API, a welding model with
// named inputs, compile once / run many, and MflError on failures.
//   node wasm/test_package.mjs      (after wasm/build.sh and wasm/package.py)
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";
import { create, MflError } from "./dist/npm/mfl.mjs";

const dir = fileURLToPath(new URL("./dist/npm/", import.meta.url));
const wasm = readFileSync(dir + "mfl.wasm");
let failures = 0;
const check = (ok, what) => { if (!ok) { failures++; console.error("FAIL " + what); } };

for (const [name, mod] of [["ESM", { create, MflError }], ["CommonJS", createRequire(import.meta.url)(dir + "mfl.cjs")]]) {
    const mfl = await mod.create(wasm);
    check(/^\d+\.\d+\.\d+/.test(mfl.version), name + ": version");
    const s = mfl.createState();
    s.addSource("game/materials/steel.mfl", "let k_s = 30 W/(m*K)\n");
    s.addSource("game/weld.mfl", [
        'import "materials/steel.mfl"', "import welding",
        'let Q = welding.heat_input(voltage: U, current: I, speed: v, efficiency: welding.efficiency("MAG"))',
        "let t85 = welding.t85_thick(heat_input: Q, conductivity: k_s, preheat: T0)",
        'print("Q =", Q.to(kJ/mm))', ""].join("\n"));
    s.setNumber("U", 25, "V");
    s.setNumber("I", 250, "A");
    s.setNumber("v", 5, "mm/s");
    s.setNumber("T0", 293.15, "K");
    const model = s.compile("game/weld.mfl");
    check(model.run() === "Q = 1 kJ/mm\n", name + ": print output");
    const exact = 1e6 / (2 * Math.PI * 30) * (1 / 480 - 1 / 780);
    check(Math.abs(s.getNumber("t85", "s") - exact) < 1e-9 * exact, name + ": t8/5");
    s.setNumber("v", 10, "mm/s");
    model.run();
    check(s.getNumber("Q", "kJ/mm") === 0.5, name + ": run many");
    let err = null;
    try { s.getNumber("Q", "s"); } catch (e) { err = e; }
    check(err instanceof mod.MflError && err.status === "MFL_ERR_UNIT", name + ": MflError on a unit mismatch");
    err = null;
    s.addSource("game/bad.mfl", "let x = 1 m + 1 s\n");
    try { s.compile("game/bad.mfl"); } catch (e) { err = e; }
    check(err && err.status === "MFL_ERR_PROGRAM" && err.message.includes("game/bad.mfl:1:13"), name + ": compile error");
    err = null;
    s.addSource("game/loop.mfl", "var i = 0\nwhile true { i = i + 1 }\n");
    try { s.compile("game/loop.mfl").run({ maxSteps: 10000 }); } catch (e) { err = e; }
    check(err && err.status === "MFL_ERR_RUNTIME" && err.message.includes("instruction limit"), name + ": instruction limit");
    model.free();
    s.free();
    console.log(name + ": ok");
}
process.exit(failures ? 1 : 0);
