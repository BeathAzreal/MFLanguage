# MFL in the browser (WebAssembly)

The interpreter compiled to WebAssembly: the same C sources as the `mfl`
command (lexer, parser, loader, checker, compiler, VM and the `math`,
`numerical` and `heat` modules), with an in-memory file system for
`import "file.mfl"`. It needs nothing from the page except one output
function, so it runs in any browser and in Node 18+.

```bash
python3 -m pip install ziglang==0.15.1   # clang + WASI libc (or any zig 0.13+)
wasm/build.sh                            # -> wasm/dist/mfl.wasm (~300 KB)
node wasm/test.mjs                       # golden tests, same output as the native CLI
```

## In an application (npm package, v0.4)

`python3 wasm/package.py` assembles `wasm/dist/npm/` (package `mfl-wasm`:
`mfl.mjs`, `mfl.cjs`, `mfl.d.ts`, `mfl.wasm`). A host that keeps a model
loaded, such as a game, uses a **state**: the C embedding API of
`include/mfl/mfl.h` in JavaScript.

```ts
import { create, MflError } from "mfl-wasm";
import wasmUrl from "mfl-wasm/mfl.wasm?url";       // Vite; or any URL / bytes

const mfl = await create(await fetch(wasmUrl));
const s = mfl.createState();
s.addSource("models/materials/steel.mfl", steelText);  // in memory: no files
s.addSource("models/weld.mfl", weldText);              // may import "materials/steel.mfl"
const weld = s.compile("models/weld.mfl");             // once; MflError with file:line:col

s.setNumber("U", 25, "V");                             // named inputs with units
s.setNumber("I", 250, "A");
s.setNumber("v", 5, "mm/s");
weld.run({ maxSteps: 1_000_000 });                     // every time the player changes something
const t85 = s.getNumber("t85", "s");                   // named results, in any unit
const Q = s.getNumber("Q", "kJ/mm");
```

Errors are `MflError` objects whose `status` is the C status name
(`MFL_ERR_PROGRAM`, `MFL_ERR_RUNTIME`, `MFL_ERR_UNIT`, ...) and whose
message is the same `file:line:col: ...` text as the `mfl` command. Inputs
are immutable variables of the model (like `let`); a model compiled once
can run any number of times with new input values of the same dimension.
`node wasm/test_package.mjs` checks the ES module and CommonJS builds;
`wasm/check_types.sh` type-checks a strict TypeScript consumer.

## Whole programs (playground)

```html
<script src="mfl.js"></script>
<script>
  const mfl = await MFL.create(await fetch("mfl.wasm"));
  mfl.addFile("lab/main.mfl", 'import heat\n...');
  const r = mfl.run("lab/main.mfl", { maxSteps: 50_000_000 });
  // r.status (0 / 1 / 3, as the mfl command), r.stdout, r.stderr,
  // r.simulation: {samples, nodes, times, positions, temperatures, at(i, j), csv, metadata}
</script>
```

- **Same results as the CLI.** `wasm/test.mjs` runs the conformance,
  error, import and heat tests and requires identical stdout and stderr,
  and requires the CSV of `examples/thermal/small_bar.mfl` to be
  byte-identical to the one the native CLI wrote. WebAssembly has no fused
  multiply-add, and the build uses `-ffp-contract=off` like the native one.
- **No files, no network.** Imports read only the files added with
  `addFile`. The simulation results are read from WebAssembly memory (as
  `Float64Array`s) or as the CSV and JSON text that `mfl --csv` would
  write; saving them is up to the page.
- **Endless loops.** `maxSteps` limits VM instructions per run; a program
  that exceeds it stops with a run-time error instead of freezing the page.
- Several states can coexist in one instance; JavaScript runs them one at
  a time. For long simulations, load the engine in a Web Worker so the
  page stays responsive.
- The C library's `stdio` is linked in (the embedding API uses it for
  exports), but no file system is provided: `mfl.js` answers the WASI
  imports with "not supported", except `fd_write`, which goes to the
  console.

Source: `src/wasm/mfl_wasm.c` (exports), `wasm/mfl.js` (loader). CI job
`wasm` builds it, runs the tests and uploads `mfl-wasm` as an artifact.
