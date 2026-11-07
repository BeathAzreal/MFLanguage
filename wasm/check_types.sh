#!/usr/bin/env bash
# Type-checks the npm package (wasm/dist/npm, from wasm/package.py) with a
# strict TypeScript consumer, as a project using it would (moduleResolution
# nodenext, package exports). Needs npm (downloads typescript once).
set -euo pipefail
cd "$(dirname "$0")"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp types_check/consumer.ts types_check/tsconfig.json "$work/"
printf '{"type": "module", "private": true}\n' > "$work/package.json"
(cd "$work" && npm install --silent --no-audit --no-fund typescript@5.6.3 > /dev/null)
cp -r dist/npm "$work/node_modules/mfl-wasm"   # after npm install, which prunes unknown packages
"$work/node_modules/.bin/tsc" -p "$work"
echo "mfl-wasm types: ok"
