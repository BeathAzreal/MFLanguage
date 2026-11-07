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
 * States (v0.4), for hosts that keep a model loaded (a game): named inputs
 * with units, compile once, run many times, named results in any unit -
 * the C embedding API of include/mfl/mfl.h:
 *
 *   const s = mfl.createState();
 *   s.addSource("models/weld.mfl", text);         // and the files it imports
 *   s.setNumber("U", 25, "V");
 *   const model = s.compile("models/weld.mfl");   // throws MflError with the messages
 *   model.run();                                  // throws MflError on a run-time error
 *   s.getNumber("t85", "s");                      // a number, in the unit asked for
 *   s.setNumber("U", 22, "V"); model.run();       // again, with new inputs
 *   model.free(); s.free();
 *
 * It is the same interpreter as the `mfl` command (same C sources, same
 * double-precision results): the CSV produced here is byte-identical to
 * the one `mfl file.mfl --csv` writes (checked by wasm/test.mjs).
 *
 * SPDX-License-Identifier: MIT
 */
(function (root) {
    "use strict";

    class MflError extends Error {
        constructor(status, message) {
            super(message);
            this.name = "MflError";
            this.status = status; /* e.g. "MFL_ERR_PROGRAM", "MFL_ERR_RUNTIME" */
        }
    }

    async function create(source) {
        let bytes = source;
        if (typeof Response !== "undefined" && source instanceof Response) {
            bytes = await source.arrayBuffer();
        }
        let sink = null;
        let stateSink = null; /* print() output of states (createState) */
        const decoder = new TextDecoder("utf-8");
        const encoder = new TextEncoder();
        let instance = null;

        const module = await WebAssembly.compile(bytes);
        /* The interpreter never needs files; the C library's stdio is wired to
         * nothing (ENOSYS) except fd_write, which goes to the console. */
        const wasi = {
            fd_prestat_get() { return 8; /* EBADF: no preopened directories */ },
            fd_write(fd, iovs, n, written) {
                const view = new DataView(instance.exports.memory.buffer);
                let total = 0;
                let text = "";
                for (let i = 0; i < n; i++) {
                    const p = view.getUint32(iovs + 8 * i, true), l = view.getUint32(iovs + 8 * i + 4, true);
                    text += decoder.decode(new Uint8Array(instance.exports.memory.buffer, p, l));
                    total += l;
                }
                if (text) (fd === 2 ? console.error : console.log)(text.replace(/\n$/, ""));
                view.setUint32(written, total, true);
                return 0;
            },
            proc_exit(code) { throw new Error("mfl: the WebAssembly module exited with code " + code); },
        };
        const imports = {
            env: {
                mfl_host_write(stream, ptr, len) {
                    const text = decoder.decode(new Uint8Array(instance.exports.memory.buffer, ptr, len));
                    if (sink !== null) {
                        (stream === 2 ? sink.err : sink.out).push(text);
                    } else if (stateSink !== null) {
                        stateSink(text);
                    }
                },
            },
            wasi_snapshot_preview1: {},
        };
        for (const imp of WebAssembly.Module.imports(module)) {
            if (imp.module === "wasi_snapshot_preview1") {
                imports.wasi_snapshot_preview1[imp.name] = wasi[imp.name] || (() => 52 /* ENOSYS */);
            }
        }
        instance = await WebAssembly.instantiate(module, imports);
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

        function putOrNull(text) {
            return text === undefined || text === null ? { ptr: 0, len: 0 } : put(String(text));
        }

        function createState(options) {
            const onPrint = (options && options.onPrint) || null;
            const S = x.mfl_wasm_state_new();
            if (!S) {
                throw new MflError("MFL_ERR_MEMORY", "out of memory");
            }
            let alive = true;
            const statusName = (st) => cstring(x.mfl_wasm_status_name(st));
            const check = (st) => {
                if (st !== 0) {
                    throw new MflError(statusName(st), cstring(x.mfl_wasm_error(S)));
                }
            };
            const live = () => {
                if (!alive) throw new Error("mfl: this state was freed");
            };
            const withStrings = (texts, fn) => {
                const ptrs = texts.map(putOrNull);
                try {
                    return fn(...ptrs);
                } finally {
                    for (const p of ptrs) if (p.ptr) x.mfl_wasm_free(p.ptr);
                }
            };
            const state = {
                addSource(path, text) {
                    live();
                    withStrings([path, text], (p, t) => check(x.mfl_wasm_add_source(S, p.ptr, t.ptr, t.len)));
                },
                setNumber(name, value, unit) {
                    live();
                    withStrings([name, unit], (n, u) => check(x.mfl_wasm_set_number(S, n.ptr, value, u.ptr)));
                },
                clearInputs() {
                    live();
                    x.mfl_wasm_clear_inputs(S);
                },
                getNumber(name, unit) {
                    live();
                    const out = x.mfl_wasm_alloc(8);
                    try {
                        withStrings([name, unit], (n, u) => check(x.mfl_wasm_get_number(S, n.ptr, u.ptr, out)));
                        return new DataView(mem()).getFloat64(out, true);
                    } finally {
                        x.mfl_wasm_free(out);
                    }
                },
                compile(path) {
                    live();
                    const sc = withStrings([path], (p) => x.mfl_wasm_compile(S, p.ptr));
                    if (!sc) {
                        throw new MflError("MFL_ERR_PROGRAM", cstring(x.mfl_wasm_error(S)));
                    }
                    let scriptAlive = true;
                    return {
                        /* Runs with the current inputs; returns the print() output. */
                        run(runOptions) {
                            live();
                            if (!scriptAlive) throw new Error("mfl: this script was freed");
                            const steps = BigInt((runOptions && runOptions.maxSteps) || 0);
                            const out = [];
                            stateSink = (text) => { out.push(text); if (onPrint) onPrint(text); };
                            try {
                                check(x.mfl_wasm_script_run(S, sc, steps));
                            } finally {
                                stateSink = null;
                            }
                            return out.join("");
                        },
                        free() {
                            if (scriptAlive) x.mfl_wasm_script_free(sc);
                            scriptAlive = false;
                        },
                    };
                },
                /* The last heat.run() of the last successful run, copied out. */
                heat1d() {
                    live();
                    const h = x.mfl_wasm_state_heat1d(S);
                    if (!h) return null;
                    const samples = x.mfl_wasm_h1_samples(h), nodes = x.mfl_wasm_h1_nodes(h);
                    const times = new Float64Array(samples), positions = new Float64Array(nodes);
                    const temperatures = new Float64Array(samples * nodes);
                    for (let i = 0; i < samples; i++) {
                        times[i] = x.mfl_wasm_h1_time(h, i);
                        temperatures.set(doubles(x.mfl_wasm_h1_profile(h, i), nodes), i * nodes);
                    }
                    for (let j = 0; j < nodes; j++) positions[j] = x.mfl_wasm_h1_position(h, j);
                    return { samples, nodes, times, positions, temperatures, at: (i, j) => temperatures[i * nodes + j] };
                },
                free() {
                    if (alive) x.mfl_wasm_state_free(S);
                    alive = false;
                },
            };
            return state;
        }

        const api = {
            version: cstring(x.mfl_wasm_version()),
            createState,

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

    const MFL = { create, MflError };
    if (typeof module !== "undefined" && module.exports) {
        module.exports = MFL;
    }
    root.MFL = MFL;
})(typeof globalThis !== "undefined" ? globalThis : this);
