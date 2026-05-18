// Checks quartz-api.js under Node: compile, host functions, values of every kind, traps, tasks, state, reload, handles.
// usage: node wasm/test-api.mjs build/wasm/wasm/quartz-api.js
import assert from "node:assert/strict";
import { resolve } from "node:path";
import { pathToFileURL } from "node:url";

const api = await import(pathToFileURL(resolve(process.argv[2])).href);
const quartz = await api.loadQuartz();
assert.equal(quartz.abiVersion, 1);

const host = "fn greet(name: str) -> str\nfn fail(code: int) -> int\nfn sum(xs: [int]) -> int\nfn names(n: int) -> [str]\n";
const plugin = `state Counter {
    total: int
}
fn add(c: Counter, n: int) -> int {
    c.total = c.total + n
    return c.total
}
fn hello(name: str, times: int) -> str {
    return greet("{name}x{times}")
}
fn divide(a: int, b: int) -> int {
    return a / b
}
fn check(code: int) -> int {
    return fail(code) + 1
}
fn total(xs: [int]) -> int {
    return sum(xs)
}
fn joined(n: int) -> str {
    var out = ""
    for s in names(n) {
        out = out + s
    }
    return out
}
fn words(n: int) -> [str] {
    var out: [str] = []
    var i = 0
    while i < n {
        out.push("w{i}")
        i += 1
    }
    return out
}
fn pair(a: int) -> (int, int) {
    return (a * 2, a + 1)
}
fn small(x: i32) -> f64 {
    return 0.5
}
`;

const program = quartz.compile({ "host.qzi": host, "plugin.qz": plugin });
assert.equal(program.function("hello").signature.result.kind, "str");
assert.ok(program.imports.some((i) => i.name === "greet" && !i.native));
const sandbox = quartz.sandbox(program, { fuelPerCall: 1_000_000 });
sandbox.bind("greet", (name) => `hi ${name}`);
sandbox.bind("fail", (code) => {
    if (code !== 0n) {
        throw new Error(`boom ${code}`);
    }
    return 0n;
});
sandbox.bind("sum", (xs, call) => {
    assert.ok(call.charge(1));
    return xs.reduce((a, b) => a + b, 0n);
});
sandbox.bind("names", (n) => Array.from({ length: Number(n) }, (_, i) => `n${i}`));
sandbox.start();

assert.equal(sandbox.call("add", 5), 5n);
assert.equal(sandbox.call("add", 7n), 12n);
assert.equal(sandbox.call("hello", "zoë ", 3), "hi zoë x3");
assert.equal(sandbox.call("total", [1, 2, 3n]), 6n);
assert.equal(sandbox.call("joined", 3), "n0n1n2");
assert.deepEqual(sandbox.call("words", 3), ["w0", "w1", "w2"]);
assert.deepEqual(sandbox.call("pair", 7), [14n, 8n]);
assert.equal(sandbox.call("small", 2), 0.5);
assert.throws(() => sandbox.call("small", 2 ** 40), RangeError);
assert.throws(() => sandbox.call("hello", 3, 3), TypeError);
assert.throws(() => sandbox.call("divide", 1), TypeError);
assert.throws(() => sandbox.call("divide", 7, 0), (e) => e instanceof api.TrapError && e.code === "divide_by_zero");
assert.throws(() => sandbox.call("check", 9),
    (e) => e.code === "host_error" && e.cause instanceof Error && e.cause.message === "boom 9");
assert.equal(sandbox.call("divide", 7, 2), 3n);
assert.throws(() => quartz.compile({ "bad.qz": 'fn f() -> int {\n    return "x"\n}\n' }), api.CompileError);

const json = sandbox.exportState();
const copy = quartz.sandbox(program);
for (const name of ["greet", "fail", "sum", "names"]) {
    copy.bind(name, () => 0n);
}
copy.start();
copy.importState(json);
assert.equal(copy.call("add", 1), 13n);
copy.close();
sandbox.close();

const tasksHost = "@handle\n@awaitable([int])\nstruct Batch {}\nfn batch() -> Batch\n@handle\nstruct Player {\n    id: int\n}\n";
const v1 = `@persist(per = Player)
state Stats {
    joins: int
}
fn join(p: Player, s: Stats) -> int {
    s.joins += 1
    return s.joins
}
suspend fn gather() -> int {
    let got = wait batch()
    var sum = 0
    for x in got {
        sum += x
    }
    return sum
}
`;
const first = quartz.compile({ "host.qzi": tasksHost, "p.qz": v1 });
const second = quartz.compile({ "host.qzi": tasksHost, "p.qz": v1.replace("s.joins += 1", "s.joins += 10") });
const tasks = quartz.sandbox(first);
tasks.bind("batch", () => 5n);
tasks.bind("Player.id", (p) => p);
tasks.start();
const waiting = tasks.spawn("gather");
assert.equal(waiting.kind, "wait");
assert.equal(tasks.liveTasks, 1);
assert.deepEqual(tasks.resume(waiting.task, [4, 5, 6]), { kind: "done", task: waiting.task, value: 15n });
assert.equal(tasks.call("join", 7), 1n);
tasks.importHandle(42, tasks.exportHandle(7));
tasks.reload(second);
assert.equal(tasks.call("join", 42), 11n);
tasks.close();
first.close();
second.close();
program.close();

const wideHost = `struct Pt {
    x: int
    y: int
}
enum Mood {
    Calm
    Loud(int)
}
@handle
@awaitable(Pt)
struct AskPt {}
@handle
@awaitable(Map::<str, [int]>)
struct AskMap {}
fn askPt() -> AskPt
fn askMap() -> AskMap
fn scale(m: Map<str, int>) -> Map<str, int>
`;
const wide = `fn counts(words: [str]) -> Map<str, int> {
    var m: Map<str, int> = Map()
    for w in words {
        m[w] = (m.get(w) ?? 0) + 1
    }
    m.remove("skip")
    return m
}
fn echo(p: Pt, m: Mood, o: int?) -> (Pt, Mood, int?) {
    return (p, m, o)
}
fn viaHost() -> int {
    var m: Map<str, int> = Map()
    m["a"] = 1
    m["b"] = 2
    return scale(m).get("b") ?? 0
}
suspend fn gather() -> str {
    let p = wait askPt()
    let m = wait askMap()
    var out = "{p.x},{p.y}"
    for k, v in m {
        out += " {k}:{v.len()}"
    }
    return out
}
`;
const wideProgram = quartz.compile({ "host.qzi": wideHost, "w.qz": wide });
const values = quartz.sandbox(wideProgram);
values.bind("askPt", () => 1n);
values.bind("askMap", () => 2n);
values.bind("scale", (m) => new Map([...m].map(([k, v]) => [k, v * 10n])));
values.start();
assert.deepEqual(values.call("counts", ["b", "skip", "a", "b"]), new Map([["b", 2n], ["a", 1n]]));
assert.deepEqual(values.call("echo", { x: 1, y: 2 }, { variant: "Loud", values: [5] }, null),
    [{ x: 1n, y: 2n }, { variant: "Loud", values: [5n] }, null]);
assert.deepEqual(values.call("echo", { x: 0, y: 0 }, "Calm", 7)[1], { variant: "Calm", values: [] });
assert.throws(() => values.call("echo", { x: 0, y: 0 }, "Sad", 7), TypeError);
assert.equal(values.call("viaHost"), 20n);
const asking = values.spawn("gather");
const next = values.resume(asking.task, { x: 3, y: 4 });
assert.equal(next.kind, "wait");
assert.deepEqual(values.resume(asking.task, new Map([["k", [1, 2]], ["j", []]])),
    { kind: "done", task: asking.task, value: "3,4 k:2 j:0" });
values.close();
wideProgram.close();
console.log("quartz-api.js: all checks passed");
