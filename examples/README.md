# Examples

All examples run (`./build/mfl <file>`); the language is described in
[`docs/language-spec.md`](../docs/language-spec.md).

| Folder | Content |
|--------|---------|
| `basic/` | Language basics |
| `thermal/` | Steel thermal properties, diffusivity and Fourier number (`steel_properties.mfl`); **v0.3**: 1D heat conduction in a bar heated at one end (`bar_heated_end.mfl`, run with `--csv out.csv` to export), a small case whose export is kept in `output/` (`small_bar.mfl`), material properties reused with `import` (`materials/carbon_steel.mfl`) |
| `welding/` | Welding heat input: `heat_input.mfl` (v0.1, prints `480 J/mm`) and `heat_input_v02.mfl` (v0.2 acceptance demonstration: function, array of speeds, loops, zero-speed branch, degC) |
| `embedding/` | C host programs: `heat_input_host.c` (target `mfl_embed_example`) runs an MFL model like a game would; `heat_bar_host.c` (target `mfl_heat_host`, v0.3) runs the heated-bar case through the API and compares it with the CLI |
