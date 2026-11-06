# Virtual calorimetry lab

A browser game in which a student finds the specific heat of an unknown
metal: pour water into a calorimeter by tilting a beaker, heat a bath with
a Bunsen burner, move the metal sample with tongs, stir, and record the
final temperature. Its physics is **`calorimetro.mfl`**, run every 0.2 s
of lab time by the MFL interpreter compiled to WebAssembly (`wasm/`); the
measured value is computed by the same model (`cp_medido`).

Each attempt is recorded (state 5 times per second plus every action) and
can be replayed. The teacher's replay shows what the student could not
see: exact volumes, poured and spilled water, the sample's real
temperature, and an evaluation of each step against the procedure.

| File | Content |
|------|---------|
| `calorimetro.mfl` | Physical model: mixing, heated bath, approach to a temperature, two-body heat exchange (exact over a step), measured specific heat |
| `check_model.mfl` | Checks of the model (CTest `run_lab_model`; also run on the WebAssembly build) |
| `web/bancada.template.html` | The game; `wasm/build_pages.py` embeds the engine and the model into `wasm/dist/bancada_calorimetria.html` |

```bash
wasm/build.sh && python3 wasm/build_pages.py   # then open wasm/dist/bancada_calorimetria.html
```

Teachers can change the model (for example the heat loss of the bath or
the effect of stirring) and rebuild the page; the game logic does not
contain any physics of its own.
