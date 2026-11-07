// test.mjs - runs the language's golden tests on the WebAssembly build and
// requires the same output as the native `mfl` command:
//
//   node wasm/test.mjs            (after wasm/build.sh)
//
// Every .mfl file of examples/ and tests/language/ is placed in the
// in-memory file system under its repository path, so imports and error
// messages use the same paths as the CTest runs from the source directory.
import { readFileSync, readdirSync, statSync, existsSync } from "node:fs";
import { join, relative } from "node:path";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";

const root = join(fileURLToPath(new URL(".", import.meta.url)), "..");
const MFL = createRequire(import.meta.url)("./mfl.js");
const mfl = await MFL.create(readFileSync(join(root, "wasm/dist/mfl.wasm")));

function walk(dir, out = []) {
    for (const name of readdirSync(dir)) {
        const p = join(dir, name);
        if (statSync(p).isDirectory()) {
            walk(p, out);
        } else if (p.endsWith(".mfl")) {
            out.push(p);
        }
    }
    return out;
}
for (const f of [...walk(join(root, "examples")), ...walk(join(root, "tests/language"))]) {
    mfl.addFile(relative(root, f).split("\\").join("/"), readFileSync(f, "utf8"));
}

const read = (p) => (existsSync(join(root, p)) ? readFileSync(join(root, p), "utf8") : "");
let passed = 0;
const failures = [];

function check(name, path, stdout, stderr, status) {
    // Match the native CTest harness on Windows checkouts with autocrlf.
    if (stdout !== null) stdout = stdout.replace(/\r\n/g, "\n");
    if (stderr !== null) stderr = stderr.replace(/\r\n/g, "\n");
    const r = mfl.run(path, { maxSteps: 50_000_000 });
    const problems = [];
    if (r.status !== status) problems.push(`status ${r.status}, expected ${status}`);
    if (stdout !== null && r.stdout !== stdout) problems.push(`stdout differs:\n${r.stdout}\n--- expected ---\n${stdout}`);
    if (stderr !== null && r.stderr !== stderr) problems.push(`stderr differs:\n${r.stderr}\n--- expected ---\n${stderr}`);
    if (problems.length) failures.push(`${name}: ${problems.join("\n")}`);
    else passed++;
    return r;
}

// Conformance tests and examples: stdout, no errors.
for (const f of readdirSync(join(root, "tests/language"))) {
    if (/^conformance_.*\.mfl$/.test(f)) {
        const base = f.slice(0, -4);
        check(base, `tests/language/${f}`, read(`tests/language/${base}.expected`), "", 0);
    }
}
for (const [path, expected] of [
    ["examples/welding/heat_input.mfl", "tests/language/run/heat_input.expected"],
    ["examples/welding/heat_input_v02.mfl", "tests/language/run/heat_input_v02.expected"],
    ["examples/thermal/steel_properties.mfl", "tests/language/run/steel_properties.expected"],
    ["examples/basic/hello.mfl", "tests/language/run/hello.expected"],
    ["examples/thermal/bar_heated_end.mfl", "tests/language/run/bar_heated_end.expected"],
    ["tests/language/heat/conformance_heat.mfl", "tests/language/heat/conformance_heat.expected"],
    ["examples/lab/check_model.mfl", "tests/language/run/lab_model.expected"],
    ["examples/welding/thermal_cycle.mfl", "tests/language/run/thermal_cycle.expected"],
]) {
    check(path, path, read(expected), "", 0);
}

// Error tests: stdout, stderr and status 1.
for (const dir of ["tests/language/errors", "tests/language/acceptance", "tests/language/heat"]) {
    for (const f of readdirSync(join(root, dir))) {
        const base = f.slice(0, -4);
        if (f.endsWith(".mfl") && existsSync(join(root, dir, base + ".stderr"))) {
            check(`${dir}/${base}`, `${dir}/${f}`, read(`${dir}/${base}.stdout`), read(`${dir}/${base}.stderr`), 1);
        }
    }
}
for (const [base, status] of [["ok", 0], ["missing", 1], ["cycle", 1], ["visibility", 1],
                              ["runtime_in_import", 1], ["duplicate", 1]]) {
    const d = "tests/language/imports";
    check(`imports/${base}`, `${d}/${base}.mfl`, read(`${d}/${base}.stdout`), read(`${d}/${base}.stderr`), status);
}

// Exported CSV: byte-identical to the one the native CLI wrote into the repository.
{
    const r = check("small_bar export", "examples/thermal/small_bar.mfl", null, "", 0);
    const want = read("examples/thermal/output/small_bar.csv");
    if (!r.simulation || r.simulation.csv !== want) {
        failures.push("small_bar export: CSV differs from examples/thermal/output/small_bar.csv");
    } else if (!r.simulation.metadata.includes('"export_complete": true')) {
        failures.push("small_bar export: metadata incomplete");
    } else {
        passed++;
    }
}

// Analytical case: values read straight from WebAssembly memory.
{
    const r = check("sine analytical", "tests/language/heat/sine_export.mfl", "21 samples\n", "", 0);
    const s = r.simulation;
    const L = 0.1, alpha = 45 / (7850 * 460);
    let worst = 0;
    for (let i = 0; i < s.samples; i++) {
        for (let j = 0; j < s.nodes; j++) {
            const x = s.positions[j], t = s.times[i];
            const exact = 300 + 50 * Math.sin(Math.PI * x / L) * Math.exp(-alpha * Math.PI ** 2 * t / L ** 2);
            worst = Math.max(worst, Math.abs(s.at(i, j) - exact));
        }
    }
    // Tolerance 0.035 K, as declared for 17 nodes in tests/numerical/test_heat1d.c.
    if (!(worst <= 0.035)) failures.push(`sine analytical: max error ${worst} K`);
    else passed++;
    console.log(`sine case: max |T - analytical| = ${worst.toExponential(3)} K`);
}

// Cooling plane wall: analytical series at t=600 s (Bi=0.125), and the
// discrete backward-Euler boundary energy balance at every stored step.
{
    const r = check("bar cooling", "examples/thermal/bar_cooling_convection.mfl", null, "", 0);
    const s = r.simulation;
    if (!s || s.nodes !== 41 || s.samples !== 61) {
        failures.push("bar cooling: missing simulation or wrong shape");
    } else {
        const metadata = JSON.parse(s.metadata);
        const center = s.at(60, 20) - 273.15, surface = s.at(60, 0) - 273.15;
        if (Math.abs(center - 384.39700827945603) > 0.3 ||
            Math.abs(surface - 362.75788546897456) > 0.3) {
            failures.push("bar cooling: analytical error exceeds 0.3 K");
        } else passed++;
        if (!metadata.method.includes("backward Euler") ||
            metadata.boundaries.left.kind !== "convection" ||
            metadata.boundaries.left.coefficient !== 100 ||
            metadata.boundaries.right.ambient_temperature !== 293.15 ||
            metadata.discretization.max_stable_time_step !== null ||
            !(metadata.discretization.ftcs_time_step_limit < 10)) {
            failures.push("bar cooling: method or convection metadata differs");
        } else passed++;
        let energyError = 0;
        for (let i = 1; i < s.samples; i++) {
            let change = 0;
            for (let j = 0; j < s.nodes; j++) {
                const weight = j === 0 || j === s.nodes - 1 ? 0.5 : 1;
                change += weight * (s.at(i, j) - s.at(i - 1, j));
            }
            change *= (0.1 / 40) * 7800 * 500;
            const loss = (s.times[i] - s.times[i - 1]) *
                100 * (s.at(i, 0) + s.at(i, 40) - 2 * 293.15);
            energyError = Math.max(energyError, Math.abs(change + loss) / Math.abs(loss));
        }
        if (energyError > 1e-9) failures.push("bar cooling: boundary energy imbalance");
        else passed++;
        console.log(`cooling: center ${center} degC, surface ${surface} degC, energy residual ${energyError}`);
    }
}

// An endless loop stops at the instruction limit instead of hanging.
{
    mfl.addFile("tmp/loop.mfl", "var i = 0\nwhile true { i = i + 1 }\n");
    const r = mfl.run("tmp/loop.mfl", { maxSteps: 100000 });
    if (r.status !== 1 || !r.stderr.includes("instruction limit")) failures.push("instruction limit not enforced");
    else passed++;
}

console.log(`MFL ${mfl.version} (WebAssembly): ${passed} passed, ${failures.length} failed`);
for (const f of failures) console.error("FAIL " + f + "\n");
process.exitCode = failures.length ? 1 : 0;
