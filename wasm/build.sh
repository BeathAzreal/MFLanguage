#!/usr/bin/env bash
# Builds MFL for the browser: wasm/dist/mfl.wasm (WebAssembly, wasm32-wasi
# reactor). Needs a clang with a WASI libc; the simplest one is Zig's:
#
#   python3 -m pip install ziglang     # or install zig 0.13+ from ziglang.org
#   wasm/build.sh
#
# Override the compiler with ZIG=/path/to/zig. The JavaScript side is
# wasm/mfl.js (browsers and Node 18+); wasm/test.mjs checks the build.
set -euo pipefail
cd "$(dirname "$0")/.."

if [ -n "${ZIG:-}" ]; then
    CC=("$ZIG" cc)
elif command -v zig > /dev/null 2>&1; then
    CC=(zig cc)
elif python3 -c "import ziglang" > /dev/null 2>&1; then
    CC=(python3 -m ziglang cc)
else
    echo "wasm/build.sh: no zig found (python3 -m pip install ziglang)" >&2
    exit 1
fi

SOURCES=(
    src/version.c src/lexer/lexer.c src/parser/ast.c src/parser/parser.c
    src/runtime/arena.c src/runtime/units.c src/runtime/number.c
    src/compiler/checker.c src/compiler/chunk.c src/compiler/compiler.c
    src/compiler/loader.c src/modules/natives.c src/modules/heat_natives.c
    src/vm/vm.c
    modules/numerical/vector.c modules/numerical/heat1d.c modules/numerical/heat1d_export.c
    src/wasm/mfl_wasm.c
)

mkdir -p wasm/dist
"${CC[@]}" -target wasm32-wasi -mexec-model=reactor -std=c11 -O2 \
    -Wall -Wextra -Wpedantic -Werror -ffp-contract=off \
    -Iinclude -Isrc -Imodules/numerical \
    -Wl,--strip-all "${SOURCES[@]}" -o wasm/dist/mfl.wasm
ls -l wasm/dist/mfl.wasm
