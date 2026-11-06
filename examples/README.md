# Examples

All examples run (`./build/mfl <file>`); the language is described in
[`docs/language-spec.md`](../docs/language-spec.md).

| Folder | Content |
|--------|---------|
| `basic/` | Language basics |
| `thermal/` | Steel thermal properties, diffusivity and Fourier number (`steel_properties.mfl`) |
| `welding/` | Welding heat input: `heat_input.mfl` (v0.1, prints `480 J/mm`) and `heat_input_v02.mfl` (v0.2 acceptance demonstration: function, array of speeds, loops, zero-speed branch, degC) |
| `embedding/` | C host program that runs an MFL model through the API, like a game would (`heat_input_host.c`, target `mfl_embed_example`) |
