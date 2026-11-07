// Type check of the npm package (wasm/check_types.sh): a strict TypeScript
// consumer that uses every public API of mfl-wasm. Compiled, never run.
import { create, MflError, type MflState, type Heat1DResult, type RunResult } from "mfl-wasm";

export async function welding(bytes: Uint8Array): Promise<number> {
    const mfl = await create(bytes);
    const s: MflState = mfl.createState({ onPrint: (text) => console.log(text) });
    s.addSource("game/weld.mfl", "import welding\n");
    s.setNumber("U", 25, "V");
    s.setNumber("efficiency", 0.8);
    const model = s.compile("game/weld.mfl");
    const printed: string = model.run({ maxSteps: 1_000_000 });
    const h: Heat1DResult | null = s.heat1d();
    if (h !== null) {
        const first: number = h.at(0, 0) + h.times[0] + h.positions[h.nodes - 1];
        console.log(first, printed);
    }
    try {
        s.getNumber("Q", "s");
    } catch (e) {
        if (e instanceof MflError) {
            console.log(e.status, e.message);
        }
    }
    const t85 = s.getNumber("t85", "s");
    model.free();
    s.free();
    mfl.addFile("a.mfl", "print(1)\n");
    const r: RunResult = mfl.run("a.mfl", { maxSteps: 1000 });
    return t85 + r.status + (r.simulation ? r.simulation.csv.length : 0);
}
