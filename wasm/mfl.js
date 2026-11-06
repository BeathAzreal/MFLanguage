/*
 * mfl.js - runs MFL programs in the browser or in Node, on the WebAssembly
 * build of the interpreter (wasm/dist/mfl.wasm, built by wasm/build.sh).
 *
 *   const mfl = await MFL.create(wasmBytes);     // ArrayBuffer / Uint8Array / Response
 *   mfl.addFile("lab/materials.mfl", "let k = 45 W/(m*K)\n");
 *   mfl.addFile("lab/main.mfl", source);
 *   const r = mfl.run("lab/main.mfl");
 *   r.status            // 0 ok, 1 errors in the program, 3 internal failure (as the mfl command)
 *   r.stdout, r.stderr  // print() output; "file:line:col: error: ..." messages
 *   r.simulation        // last heat.run(): {samples, nodes, times, positions,
 *                       //   temperatures (Float64Array, K, sample by sample),
 *                       //   at(sample, node), csv, metadata} - or null
 *
 * It is the same interpreter as the `mfl` command (same C sources, same
 * double-precision results): the CSV produced here is byte-identical to
 * the one `mfl file.mfl --csv` writes (checked by wasm/test.mjs).
 *
 * SPDX-License-Identifier: MIT
 */
(function (root) {
    "use strict";

    async function create(source) {
        let bytes = source;
        if (typeof Response !== "undefined" && source instanceof Response) {
            bytes = await source.arrayBuffer();
        }
        let sink = null;
        const decoder = new TextDecoder("utf-8");
        const encoder = new TextEncoder();
        let instance = null;

        const imports = {
            env: {
                mfl_host_write(stream, ptr, len) {
                    if (sink === null) {
                        return;
                    }
                    const text = decoder.decode(new Uint8Array(instance.exports.memory.buffer, ptr, len));
                    (stream === 2 ? sink.err : sink.out).push(text);
                },
            },
        };
        const result = await WebAssembly.instantiate(bytes, imports);
        instance = result.instance || result;
        const x = instance.exports;
        if (x._initialize) {
            x._initialize();
        }

        function mem() {
            return x.memory.buffer;
        }

        /* Copies a JS string into WebAssembly memory (NUL-terminated). */
        function put(text) {
            const data = encoder.encode(text);
            const ptr = x.mfl_wasm_alloc(data.length + 1);
            const view = new Uint8Array(mem(), ptr, data.length + 1);
            view.set(data);
            view[data.length] = 0;
            return { ptr, len: data.length };
        }

        function cstring(ptr, len) {
            if (!ptr) {
                return "";
            }
            if (len === undefined) {
                const view = new Uint8Array(mem());
                len = 0;
                while (view[ptr + len] !== 0) {
                    len++;
                }
            }
            return decoder.decode(new Uint8Array(mem(), ptr, len));
        }

        function doubles(ptr, n) {
            return ptr ? new Float64Array(mem().slice(ptr, ptr + 8 * n)) : new Float64Array(0);
        }

        const api = {
            version: cstring(x.mfl_wasm_version()),

            /* Adds or replaces a file of the in-memory file system. Paths use
             * '/', like import paths; imports resolve relative to the
             * importing file. */
            addFile(path, text) {
                const p = put(path);
                const d = put(text);
                const ok = x.mfl_wasm_add_file(p.ptr, d.ptr, d.len);
                x.mfl_wasm_free(p.ptr);
                x.mfl_wasm_free(d.ptr);
                if (!ok) {
                    throw new Error("mfl: out of memory while adding " + path);
                }
            },

            clearFiles() {
                x.mfl_wasm_clear_files();
            },

            /* Runs a file. options.maxSteps limits VM instructions (0 = none),
             * so an endless loop cannot freeze the page. */
            run(path, options) {
                const maxSteps = BigInt((options && options.maxSteps) || 0);
                const out = [];
                const err = [];
                sink = { out, err };
                const p = put(path);
                let status;
                try {
                    status = x.mfl_wasm_run(p.ptr, maxSteps);
                } finally {
                    x.mfl_wasm_free(p.ptr);
                    sink = null;
                }
                const samples = x.mfl_wasm_sim_samples();
                const nodes = x.mfl_wasm_sim_nodes();
                let simulation = null;
                if (samples > 0) {
                    const temperatures = doubles(x.mfl_wasm_sim_temperatures(), samples * nodes);
                    simulation = {
                        samples,
                        nodes,
                        times: doubles(x.mfl_wasm_sim_times(), samples),
                        positions: doubles(x.mfl_wasm_sim_positions(), nodes),
                        temperatures,
                        at(sample, node) {
                            return temperatures[sample * nodes + node];
                        },
                        csv: cstring(x.mfl_wasm_sim_csv(), x.mfl_wasm_sim_csv_length()),
                        metadata: cstring(x.mfl_wasm_sim_metadata(), x.mfl_wasm_sim_metadata_length()),
                    };
                }
                return { status, stdout: out.join(""), stderr: err.join(""), simulation };
            },
        };
        return api;
    }

    const MFL = { create };
    if (typeof module !== "undefined" && module.exports) {
        module.exports = MFL;
    }
    root.MFL = MFL;
})(typeof globalThis !== "undefined" ? globalThis : this);
