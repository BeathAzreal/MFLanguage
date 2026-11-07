#!/usr/bin/env python3
"""Assembles the npm package of the browser build in wasm/dist/npm/:

    mfl.wasm   the interpreter (wasm/build.sh)
    mfl.mjs    ES module   (import { create } from "mfl-wasm")
    mfl.cjs    CommonJS    (require("mfl-wasm"))
    mfl.d.ts   TypeScript declarations
    README.md, LICENSE, package.json

Load the engine with, for example (Vite):
    import { create } from "mfl-wasm";
    import wasmUrl from "mfl-wasm/mfl.wasm?url";
    const mfl = await create(await fetch(wasmUrl));
"""
import json, pathlib, re, shutil

root = pathlib.Path(__file__).resolve().parent.parent
out = root / "wasm/dist/npm"
out.mkdir(parents=True, exist_ok=True)
js = (root / "wasm/mfl.js").read_text(encoding="utf-8")
version = re.search(r'#define MFL_VERSION_STRING "([^"]+)"', (root / "include/mfl/mfl.h").read_text()).group(1)

# The core is one IIFE; give it a private root object instead of globalThis.
core = js.replace('})(typeof globalThis !== "undefined" ? globalThis : this);', "})(__mfl_root);")
assert core != js, "mfl.js footer changed"
(out / "mfl.mjs").write_text("const __mfl_root = {};\nconst module = undefined;\n" + core +
    "\nconst MFL = __mfl_root.MFL;\nexport const create = MFL.create;\nexport const MflError = MFL.MflError;\nexport default MFL;\n",
    encoding="utf-8")
(out / "mfl.cjs").write_text("const __mfl_root = {};\n" + core + "\nmodule.exports = __mfl_root.MFL;\n", encoding="utf-8")
shutil.copy(root / "wasm/mfl.d.ts", out / "mfl.d.ts")
shutil.copy(root / "wasm/dist/mfl.wasm", out / "mfl.wasm")
shutil.copy(root / "wasm/README.md", out / "README.md")
shutil.copy(root / "LICENSE", out / "LICENSE")
(out / "package.json").write_text(json.dumps({
    "name": "mfl-wasm",
    "version": version,
    "description": "MFL (Materials & Manufacturing Language) interpreter compiled to WebAssembly",
    "license": "MIT",
    "type": "module",
    "main": "./mfl.cjs",
    "module": "./mfl.mjs",
    "types": "./mfl.d.ts",
    "exports": {
        ".": {"types": "./mfl.d.ts", "import": "./mfl.mjs", "require": "./mfl.cjs"},
        "./mfl.wasm": "./mfl.wasm",
        "./package.json": "./package.json"
    },
    "files": ["mfl.mjs", "mfl.cjs", "mfl.d.ts", "mfl.wasm", "README.md", "LICENSE"],
    "repository": "https://github.com/BeathAzreal/MFLanguage"
}, indent=2) + "\n", encoding="utf-8")
print("wasm/dist/npm:", ", ".join(sorted(p.name for p in out.iterdir())))
