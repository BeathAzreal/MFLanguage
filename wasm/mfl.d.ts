/**
 * MFL in the browser and in Node: the MFL interpreter compiled to
 * WebAssembly (see wasm/README.md). Types for mfl.mjs / mfl.cjs.
 */

/** Raised by state operations; `status` is the C API status name. */
export declare class MflError extends Error {
    readonly status:
        | "MFL_ERR_ARGUMENT" | "MFL_ERR_MEMORY" | "MFL_ERR_PROGRAM" | "MFL_ERR_UNIT"
        | "MFL_ERR_NOT_FOUND" | "MFL_ERR_TYPE" | "MFL_ERR_INPUTS_CHANGED" | "MFL_ERR_NOT_RUN"
        | "MFL_ERR_INTERNAL" | "MFL_ERR_RUNTIME" | "MFL_ERR_SIMULATION" | "MFL_ERR_IO"
        | string;
}

/** Temperatures in K, sample by sample: `temperatures[sample * nodes + node]`. */
export interface Heat1DResult {
    readonly samples: number;
    readonly nodes: number;
    /** s */
    readonly times: Float64Array;
    /** m */
    readonly positions: Float64Array;
    /** K */
    readonly temperatures: Float64Array;
    at(sample: number, node: number): number;
}

export interface RunOptions {
    /** Stop after this many VM instructions (0 = no limit), so a loop cannot freeze the page. */
    maxSteps?: number;
}

/** A model compiled once and run many times with new inputs. */
export interface MflScript {
    /** Runs with the current inputs; returns the print() output. Throws MflError. */
    run(options?: RunOptions): string;
    free(): void;
}

/** An interpreter state: inputs, sources, results of the last run. */
export interface MflState {
    /** Adds (or replaces) a .mfl source; `import "x.mfl"` finds it relative to the importer. */
    addSource(path: string, text: string): void;
    /** A named input with an MFL unit ("V", "mm/s", "W/(m*K)"); no unit = plain number. */
    setNumber(name: string, value: number, unit?: string): void;
    clearInputs(): void;
    /** Checks and compiles an added source. Throws MflError with file:line:col messages. */
    compile(path: string): MflScript;
    /** A top-level variable of the last successful run, in `unit` (default: coherent SI). */
    getNumber(name: string, unit?: string): number;
    /** The last heat.run() of the last successful run, or null. */
    heat1d(): Heat1DResult | null;
    free(): void;
}

export interface Simulation extends Heat1DResult {
    /** CSV as `mfl --csv` writes it (time_s,position_m,temperature_K). */
    readonly csv: string;
    /** JSON metadata as `mfl --csv` writes it. */
    readonly metadata: string;
}

export interface RunResult {
    /** 0 success, 1 errors in the program, 3 internal failure (as the mfl command). */
    status: number;
    stdout: string;
    stderr: string;
    simulation: Simulation | null;
}

export interface Mfl {
    /** Library version, e.g. "0.4.0-dev". */
    readonly version: string;
    /** A new independent state (several may coexist). */
    createState(options?: { onPrint?: (text: string) => void }): MflState;
    /** Whole-program runs, like the mfl command, on an in-memory file system. */
    addFile(path: string, text: string): void;
    clearFiles(): void;
    run(path: string, options?: RunOptions): RunResult;
}

/** Instantiates the engine from the bytes of mfl.wasm (or a fetch Response). */
export declare function create(wasm: ArrayBuffer | Uint8Array | Response): Promise<Mfl>;

declare const MFL: { create: typeof create; MflError: typeof MflError };
export default MFL;
