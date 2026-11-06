# Examples

All examples run with the v0.1 core (`./build/mfl <file>`); the syntax is
described in [`docs/language-spec.md`](../docs/language-spec.md).

| Folder | Content |
|--------|---------|
| `basic/` | Language basics |
| `thermal/` | Steel thermal properties, diffusivity and Fourier number (`steel_properties.mfl`) |
| `welding/` | Welding heat input (`heat_input.mfl`, prints `480 J/mm`) |
| `embedding/` | C host program that runs an MFL model through the API, like a game would (`heat_input_host.c`, target `mfl_embed_example`) |
