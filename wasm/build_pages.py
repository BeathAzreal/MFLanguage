#!/usr/bin/env python3
"""Builds the single-file web pages that embed the MFL WebAssembly engine:

    wasm/dist/mfl_playground.html        (wasm/playground.template.html)
    wasm/dist/bancada_calorimetria.html  (examples/lab/web/bancada.template.html)

Run wasm/build.sh first. Each page inlines wasm/mfl.js, the .wasm (base64)
and the .mfl files it needs, so it works as one file (no server)."""
import base64, json, pathlib

root = pathlib.Path(__file__).resolve().parent.parent
read = lambda p: (root / p).read_text(encoding="utf-8")
js = read("wasm/mfl.js").replace("</script", "<\\/script")
wasm = base64.b64encode((root / "wasm/dist/mfl.wasm").read_bytes()).decode()
as_js = lambda v: json.dumps(v).replace("</", "<\\/")

def page(template, out, extra):
    text = read(template).replace("/*MFL_JS*/", js).replace('"/*WASM_B64*/"', '"' + wasm + '"')
    for key, value in extra.items():
        text = text.replace(key, as_js(value))
    (root / out).write_text(text, encoding="utf-8")
    print(out, len(text), "bytes")

files = lambda *paths: {p: read(p) for p in paths}
examples = [
    {"id": "bar", "title": "Bar heated at one end (heat module)", "main": "examples/thermal/bar_heated_end.mfl",
     "files": files("examples/thermal/bar_heated_end.mfl", "examples/thermal/materials/carbon_steel.mfl")},
    {"id": "sine", "title": "Analytical check (sine profile)", "main": "tests/language/heat/sine_export.mfl",
     "files": files("tests/language/heat/sine_export.mfl")},
    {"id": "q", "title": "Welding heat input (480 J/mm)", "main": "examples/welding/heat_input.mfl",
     "files": files("examples/welding/heat_input.mfl")},
    {"id": "v02", "title": "Functions, arrays and degC (v0.2 demo)", "main": "examples/welding/heat_input_v02.mfl",
     "files": files("examples/welding/heat_input_v02.mfl")},
    {"id": "err", "title": "A dimension error, caught before running", "main": "lab/mistake.mfl",
     "files": {"lab/mistake.mfl": "# Passing a power where a speed is expected is rejected before running.\n"
               "fn heat_input(U: Quantity[V], I: Quantity[A], v: Quantity[mm/s]) -> Quantity[J/mm] {\n"
               "    return 0.8 * U * I / v\n}\nlet P = 3 kW\nprint(heat_input(20 V, 150 A, P))\n"}},
]
page("wasm/playground.template.html", "wasm/dist/mfl_playground.html", {"/*EXAMPLES*/": examples})
page("examples/lab/web/bancada.template.html", "wasm/dist/bancada_calorimetria.html",
     {"/*MODEL*/": read("examples/lab/calorimetro.mfl")})
