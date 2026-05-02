// Runs tests/programs/*.qz through quartz.js under Node and checks their `// expect:`, `// trap:` and `// warning:` lines.
// usage: node wasm/check-programs.mjs build/wasm/wasm/quartz.js tests/programs
import { createRequire } from "node:module";
import { readdirSync, readFileSync, existsSync } from "node:fs";
import { join, resolve } from "node:path";

const [modulePath, dir] = process.argv.slice(2);
const createQuartz = createRequire(import.meta.url)(resolve(modulePath));
let sink = null;
const qz = await createQuartz({ onEmit: (stream, text) => sink.push([stream, text]) });

const directives = (text, key) =>
    text.split("\n").filter((l) => l.startsWith(`// ${key}:`)).map((l) => l.slice(key.length + 4).replace(/^ /, ""));

let failed = 0;
for (const file of readdirSync(dir).filter((f) => f.endsWith(".qz")).sort()) {
    const path = join(dir, file);
    const text = readFileSync(path, "utf8");
    const inputs = [[path, ""]];
    for (const line of text.split("\n")) {
        if (line.startsWith("// file: ")) {
            inputs.push([line.slice(9), ""]);
        } else {
            inputs[inputs.length - 1][1] += line + "\n";
        }
    }
    const iface = path.replace(/\.qz$/, ".qzi");
    if (existsSync(iface)) {
        inputs.push([iface, readFileSync(iface, "utf8")]);
    }
    sink = [];
    qz.ccall("qzp_reset", null, [], []);
    for (const [name, source] of inputs) {
        qz.ccall("qzp_source", null, ["string", "string"], [name, source]);
    }
    const outcome = qz.ccall("qzp_run", "number", ["string", "number", "number", "number"], ["main", 1e8, 1e9, 0]);
    const printed = sink.filter(([s]) => s === 0).map(([, t]) => t + "\n").join("");
    const traps = sink.filter(([s]) => s === 2).map(([, t]) => t).join("\n");
    const diagnostics = sink.filter(([s]) => s === 1).map(([, t]) => t).join("");
    const expected = directives(text, "expect").map((l) => l + "\n").join("");
    const wantTrap = directives(text, "trap");
    const problems = [];
    if (printed !== expected) {
        problems.push(`printed\n${printed}expected\n${expected}`);
    }
    if (wantTrap.length > 0 ? !wantTrap.every((want) => traps.includes(want)) : outcome !== 0) {
        problems.push(`outcome ${outcome}: ${traps || diagnostics}`);
    }
    for (const want of directives(text, "warning")) {
        if (!diagnostics.includes(want)) {
            problems.push(`missing warning: ${want}`);
        }
    }
    const lines = printed.split("\n").length - 1;
    console.log(`${problems.length === 0 ? "ok  " : "FAIL"} ${file} (${lines} lines)`);
    for (const p of problems) {
        console.log(p);
    }
    failed += problems.length > 0 ? 1 : 0;
}
process.exit(failed === 0 ? 0 : 1);
