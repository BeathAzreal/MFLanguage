# MFL in the browser (WebAssembly)

The interpreter compiled to WebAssembly: the same C sources as the `mfl`
command (lexer, parser, loader, checker, compiler, VM and the `math`,
`numerical` and `heat` modules), with an in-memory file system for
`import "file.mfl"`. It needs nothing from the page except one output
function, so it runs in any browser and in Node 18+.

```bash
python3 -m pip install ziglang==0.15.1   # clang + WASI libc (or any zig 0.13+)
wasm/build.sh                            # -> wasm/dist/mfl.wasm (~260 KB)
node wasm/test.mjs                       # golden tests, same output as the native CLI
```

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
- One module instance runs one program at a time; create several
  instances (or use a Web Worker) for parallel runs.

Source: `src/wasm/mfl_wasm.c` (exports), `wasm/mfl.js` (loader). CI job
`wasm` builds it, runs the tests and uploads `mfl-wasm` as an artifact.
