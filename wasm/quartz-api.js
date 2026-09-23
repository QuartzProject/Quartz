// The embedding API of Quartz for JavaScript, over quartz-capi.js (the C interface built with Emscripten).
// Types and documentation are in quartz-api.d.ts.

import { Values } from "./quartz-values.js";

export const Kind = Object.freeze(["void", "bool", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64",
    "char", "str", "bytes", "list", "map", "struct", "enum", "nullable", "tuple", "handle", "function", "other"]);
export const TrapCode = Object.freeze(["none", "unreachable", "divide_by_zero", "index_out_of_bounds", "null_unwrap",
    "out_of_fuel", "out_of_memory", "stack_overflow", "invalid_handle", "panic", "host_error"]);
const Tier = { default: 0, interpreter: 1, automatic: 2, native: 3 };
const OK = 0;
const TRAP = 2;

export class QuartzError extends Error {
    constructor(message, options) {
        super(message, options);
        this.name = "QuartzError";
    }
}

export class CompileError extends QuartzError {
    constructor(diagnostics, messages = []) {
        super(diagnostics.trim() || "compilation failed");
        this.name = "CompileError";
        this.diagnostics = diagnostics;
        this.messages = messages;
    }
}

const Severity = Object.freeze(["error", "warning", "note"]);
const Level = { ignore: 0, warning: 1, error: 2 };

export class TrapError extends QuartzError {
    constructor(code, message, cause) {
        super(message, cause === undefined ? undefined : { cause });
        this.name = "TrapError";
        this.code = code;
    }
}

/** Loads the WebAssembly module and returns the API; in a browser, pass `createModule` (createQuartzCApi). */
export async function loadQuartz(options = {}) {
    let create = options.createModule;
    if (create === undefined) {
        const { createRequire } = await import("node:module");
        create = createRequire(import.meta.url)("./quartz-capi.js");
    }
    const settings = options.locateFile === undefined ? {} : { locateFile: options.locateFile };
    return new Quartz(await create(settings));
}

const encoder = new TextEncoder();

class Quartz {
    #m;
    #bindings = new Map();
    #nextBinding = 1;
    #dispatch;

    constructor(module) {
        this.#m = module;
        this.scratch = module._malloc(64);
        const host = (call, args, result, user) => this.#host(call >>> 0, args >>> 0, result >>> 0, user);
        this.#dispatch = module.addFunction(host, "iiiii");
    }

    get module() {
        return this.#m;
    }

    get abiVersion() {
        return this.#m._qz_abi_version();
    }

    /** Compiles sources given as `{ name: text }` or `[{ name, text }]`; throws CompileError. */
    compile(sources, options = {}) {
        const list = Array.isArray(sources) ? sources : Object.entries(sources).map(([name, text]) => ({ name, text }));
        const m = this.#m;
        const owned = [];
        const names = m._malloc(4 * list.length + 4);
        const texts = m._malloc(4 * list.length + 4);
        const lengths = m._malloc(4 * list.length + 4);
        owned.push(names, texts, lengths);
        list.forEach(({ name, text }, i) => {
            const n = this.bytesIn(encoder.encode(name + "\0"));
            const bytes = encoder.encode(text);
            const t = this.bytesIn(bytes);
            owned.push(n, t);
            this.i32(names + 4 * i, n);
            this.i32(texts + 4 * i, t);
            this.i32(lengths + 4 * i, bytes.length);
        });
        const manifest = options.manifest == null ? 0 : this.cString(options.manifest);
        owned.push(manifest);
        const flags = (options.emitBinary ? 1 : 0) | (options.keepLines === false ? 0 : 2);
        const rules = Object.entries(options.rules ?? {});
        const rulePtrs = m._malloc(4 * rules.length + 4);
        const levels = m._malloc(4 * rules.length + 4);
        owned.push(rulePtrs, levels);
        rules.forEach(([rule, level], i) => {
            if (!(level in Level)) {
                throw new RangeError(`a rule level is ignore, warning or error, not ${level}`);
            }
            const r = this.cString(rule);
            owned.push(r);
            this.i32(rulePtrs + 4 * i, r);
            this.i32(levels + 4 * i, Level[level]);
        });
        this.i32(this.scratch, 0);
        const p = m._qzw_compile_with(names, texts, lengths, list.length, flags, manifest, rulePtrs, levels,
            rules.length, this.scratch) >>> 0;
        owned.forEach((ptr) => ptr && m._free(ptr));
        const { diagnostics, messages } = this.#diagnostics(this.u32(this.scratch));
        if (p === 0) {
            throw new CompileError(diagnostics, messages);
        }
        const program = new Program(this, p, diagnostics);
        program.messages = messages;
        return program;
    }

    #diagnostics(d) {
        const m = this.#m;
        const messages = [];
        for (let i = 0; i < (m._qz_diagnostics_count(d) >>> 0); ++i) {
            m._qz_diagnostic_range(d, i, this.scratch, this.scratch + 4, this.scratch + 8, this.scratch + 12);
            messages.push({
                severity: Severity[m._qz_diagnostic_severity(d, i)],
                rule: m.UTF8ToString(m._qz_diagnostic_rule(d, i)),
                file: m.UTF8ToString(m._qz_diagnostic_file(d, i)),
                line: this.u32(this.scratch),
                column: this.u32(this.scratch + 4),
                endLine: this.u32(this.scratch + 8),
                endColumn: this.u32(this.scratch + 12),
                message: m.UTF8ToString(m._qz_diagnostic_message(d, i)),
            });
        }
        const diagnostics = d === 0 ? "" : m.UTF8ToString(m._qz_diagnostics_text(d));
        m._qz_diagnostics_free(d);
        return { diagnostics, messages };
    }

    /** Loads a .qzb module, checking its hash and, with trusted Ed25519 keys, its signature. */
    load(bytes, options = {}) {
        const m = this.#m;
        const keys = options.trustedKeys ?? [];
        const module = this.bytesIn(bytes);
        const keyBytes = m._malloc(32 * keys.length + 1);
        keys.forEach((k, i) => {
            if (k.length !== 32) {
                throw new RangeError("an Ed25519 public key has 32 bytes");
            }
            m.HEAPU8.set(k, keyBytes + 32 * i);
        });
        this.i32(this.scratch, 0);
        const p = m._qz_load(module, bytes.length, keys.length ? keyBytes : 0, keys.length, !!options.requireSignature,
            this.scratch) >>> 0;
        m._free(module);
        m._free(keyBytes);
        const error = this.take(this.u32(this.scratch));
        if (p === 0) {
            throw new QuartzError(error ?? "cannot load the module");
        }
        return new Program(this, p, "");
    }

    /** A sandbox of `program` with its own memory, fuel and state. */
    sandbox(program, limits = {}) {
        return new Sandbox(this, program, limits);
    }

    bind(sandbox, name, binding) {
        const id = this.#nextBinding++;
        this.#bindings.set(id, binding);
        const n = this.cString(name);
        const ok = this.#m._qz_sandbox_bind_direct(sandbox, n, this.#dispatch, id);
        this.#m._free(n);
        if (!ok) {
            this.#bindings.delete(id);
            return 0;
        }
        return id;
    }

    unbind(id) {
        this.#bindings.delete(id);
    }

    #host(call, args, result, user) {
        const binding = this.#bindings.get(user);
        if (binding === undefined) {
            return 0;
        }
        return binding.sandbox.runHost(binding, call, args, result) ? 1 : 0;
    }

    view() {
        return new DataView(this.#m.HEAPU8.buffer);
    }

    i32(ptr, value) {
        this.view().setInt32(ptr, value, true);
    }

    u32(ptr) {
        return this.view().getUint32(ptr, true);
    }

    slot(ptr) {
        return this.view().getBigUint64(ptr, true);
    }

    setSlot(ptr, bits) {
        this.view().setBigUint64(ptr, BigInt.asUintN(64, bits), true);
    }

    bytesIn(bytes) {
        const ptr = this.#m._malloc(bytes.length + 1);
        this.#m.HEAPU8.set(bytes, ptr);
        return ptr;
    }

    cString(text) {
        return this.bytesIn(encoder.encode(text + "\0"));
    }

    /** Copies a NUL-terminated string the library allocated and frees it. */
    take(ptr) {
        if (ptr === 0) {
            return null;
        }
        const text = this.#m.UTF8ToString(ptr);
        this.#m._qz_free(ptr);
        return text;
    }

    /** The bytes a str slot points to. */
    strBytes(bits) {
        this.setSlot(this.scratch, bits);
        const data = this.#m._qzw_str(this.scratch, this.scratch + 8) >>> 0;
        const length = this.u32(this.scratch + 8);
        return this.#m.HEAPU8.slice(data, data + length);
    }

    type(program, id, cache = new Map()) {
        if (cache.has(id)) {
            return cache.get(id);
        }
        const m = this.#m;
        const kind = Kind[m._qz_type_kind(program, id, this.scratch)] ?? "other";
        const type = { kind, slots: this.u32(this.scratch), id };
        cache.set(id, type);
        const element = m._qz_type_elem(program, id);
        if (element !== 0) {
            type.element = this.type(program, element, cache);
        }
        if (kind === "map") {
            type.key = this.type(program, m._qz_type_key(program, id), cache);
        } else if (kind === "nullable") {
            type.pointer = !!m._qz_type_nullable_is_pointer(program, id);
        } else if (kind === "struct" || kind === "tuple") {
            type.fields = [];
            for (let i = 0; i < m._qz_type_field_count(program, id); ++i) {
                const field = m._qz_type_field(program, id, i, this.scratch);
                const offset = this.u32(this.scratch);
                const name = kind === "struct" ? m.UTF8ToString(m._qz_type_field_name(program, id, i)) : undefined;
                type.fields.push({ name, offset, type: this.type(program, field, cache) });
            }
        } else if (kind === "enum") {
            type.variants = [];
            for (let v = 0; v < m._qz_type_variant_count(program, id); ++v) {
                const payload = [];
                for (let k = 0; k < m._qz_type_variant_payload_count(program, id, v); ++k) {
                    const p = m._qz_type_variant_payload(program, id, v, k, this.scratch);
                    payload.push({ offset: this.u32(this.scratch), type: this.type(program, p, cache) });
                }
                type.variants.push({ name: m.UTF8ToString(m._qz_type_variant_name(program, id, v)), payload });
            }
        }
        return type;
    }
}

class Program {
    #q;
    #address;
    #exports = new Map();
    #imports;

    constructor(quartz, address, diagnostics) {
        this.#q = quartz;
        this.#address = address;
        this.diagnostics = diagnostics;
        this.messages = [];
    }

    get address() {
        if (this.#address === 0) {
            throw new QuartzError("program is closed");
        }
        return this.#address;
    }

    /** The function called `name` with its signature, or undefined. */
    function(name) {
        if (!this.#exports.has(name)) {
            const m = this.#q.module;
            const n = this.#q.cString(name);
            const index = m._qz_program_function(this.address, n) | 0;
            m._free(n);
            let found;
            if (index !== -1) {
                const p = this.address;
                const params = [];
                for (let i = 0; i < m._qz_program_function_arity(p, index); ++i) {
                    params.push(this.#q.type(p, m._qz_program_function_param_type(p, index, i)));
                }
                const result = this.#q.type(p, m._qz_program_function_result_type(p, index));
                found = { name, index, signature: { params, result } };
            }
            this.#exports.set(name, found);
        }
        return this.#exports.get(name);
    }

    /** What the program imports; `native` ones the runtime binds itself. */
    get imports() {
        if (this.#imports === undefined) {
            const m = this.#q.module;
            const p = this.address;
            this.#imports = [];
            for (let i = 0; i < m._qz_program_import_count(p); ++i) {
                const params = [];
                for (let k = 0; k < m._qz_program_import_arity(p, i); ++k) {
                    params.push(this.#q.type(p, m._qz_program_import_param_type(p, i, k)));
                }
                const result = this.#q.type(p, m._qz_program_import_result_type(p, i));
                this.#imports.push({ name: m.UTF8ToString(m._qz_program_import_name(p, i)), index: i,
                    native: !!m._qz_program_import_is_native(p, i), required: !!m._qz_program_import_is_required(p, i),
                    signature: { params, result } });
            }
        }
        return this.#imports;
    }

    /** The .qzb bytes, when compiled with `emitBinary` or loaded; null otherwise. */
    binary() {
        const m = this.#q.module;
        const data = m._qz_program_binary(this.address, this.#q.scratch) >>> 0;
        if (data === 0) {
            return null;
        }
        const out = m.HEAPU8.slice(data, data + this.#q.u32(this.#q.scratch));
        m._qz_free(data);
        return out;
    }

    /** The .qzi other packages compile against; `manifest` is package.toml text. */
    packageInterface(manifest) {
        const m = this.#q.module;
        const text = manifest == null ? 0 : this.#q.cString(manifest);
        this.#q.i32(this.#q.scratch, 0);
        const out = m._qz_program_interface(this.address, text, this.#q.scratch);
        if (text) {
            m._free(text);
        }
        const error = this.#q.take(this.#q.u32(this.#q.scratch));
        const result = this.#q.take(out);
        if (result === null) {
            throw new QuartzError(error ?? "the program has no package interface");
        }
        return result;
    }

    /** Frees the program; sandboxes made from it keep working. */
    close() {
        if (this.#address !== 0) {
            this.#q.module._qz_program_free(this.#address);
            this.#address = 0;
        }
    }
}

class Sandbox {
    #q;
    #address;
    #program;
    #bindings = [];
    #tasks = new Map();
    #args = 0;
    #argCapacity = 0;
    #active = false;
    #failure;
    #values;

    constructor(quartz, program, limits) {
        this.#q = quartz;
        this.#program = program;
        this.#values = new Values(quartz, this);
        const q = quartz;
        q.setSlot(q.scratch, BigInt(limits.fuelPerCall ?? 0));
        const tier = Tier[limits.tier ?? "default"];
        if (tier === undefined) {
            throw new RangeError(`unknown tier ${limits.tier}`);
        }
        this.#address = q.module._qzw_sandbox_new(program.address, limits.memoryBytes ?? 0, q.scratch,
            limits.maxDepth ?? 0, tier) >>> 0;
        if (this.#address === 0) {
            throw new QuartzError("cannot create a sandbox");
        }
    }

    get program() {
        return this.#program;
    }

    get address() {
        if (this.#address === 0) {
            throw new QuartzError("sandbox is closed");
        }
        return this.#address;
    }

    /** Binds the host import `name`; `fn` gets the arguments decoded and, last, a call with `charge(units)`. */
    bind(name, fn) {
        const declared = this.#program.imports.find((i) => i.name === name);
        if (declared === undefined) {
            throw new RangeError(`the program imports nothing named ${name}`);
        }
        const binding = { sandbox: this, fn, name, signature: declared.signature };
        binding.id = this.#q.bind(this.address, name, binding);
        if (binding.id === 0) {
            throw new RangeError(`the program imports nothing named ${name}`);
        }
        this.#bindings.push(binding);
    }

    start() {
        this.#run(() => this.#check(this.#q.module._qz_sandbox_start(this.address)));
    }

    /** Calls an exported function; arguments are checked against its signature. Traps throw TrapError. */
    call(name, ...args) {
        return this.#run(() => {
            const f = this.#function(name);
            const m = this.#q.module;
            const n = this.#argv(f.signature, args);
            const width = Math.max(1, f.signature.result.slots);
            const out = m._malloc(8 * width);
            try {
                const status = width === 1 ? m._qz_sandbox_call(this.address, f.index, this.#args, n, out)
                    : m._qz_sandbox_call_wide(this.address, f.index, this.#args, n, out, width);
                this.#check(status);
                return this.#values.decodeAt(f.signature.result, out);
            } finally {
                m._free(out);
            }
        });
    }

    /** Starts a task (a `suspend fn`); returns `{ kind, task, value }`. */
    spawn(name, ...args) {
        return this.#run(() => {
            const f = this.#function(name);
            const n = this.#argv(f.signature, args);
            this.#check(this.#q.module._qz_sandbox_spawn(this.address, f.index, this.#args, n, this.#q.scratch + 16));
            this.#tasks.set(this.#q.u32(this.#q.scratch + 20), f.signature.result);
            return this.#outcome();
        });
    }

    /** Resumes a task with what its `wait` produces, checked against the awaited type. */
    resume(task, value) {
        return this.#run(() => {
            const m = this.#q.module;
            const type = this.#q.type(this.#program.address, m._qz_sandbox_wait_type(this.address, task));
            const slots = type.kind === "void" ? 0 : type.slots;
            const cell = m._malloc(8 * Math.max(1, slots));
            try {
                if (slots !== 0) {
                    this.#values.encodeInto(type, value, false, cell);
                }
                this.#check(m._qz_sandbox_resume_wide(this.address, task, cell, slots, this.#q.scratch + 16));
            } finally {
                m._free(cell);
            }
            return this.#outcome();
        });
    }

    cancel(task) {
        this.#run(() => {
            this.#tasks.delete(task);
            this.#q.module._qz_sandbox_cancel(this.address, task);
        });
    }

    /** Every state as JSON. */
    exportState() {
        return this.#run(() => this.#json(this.#q.module._qz_sandbox_export_state(this.address)));
    }

    importState(json) {
        this.#run(() => this.#withText(json, (p, n) => this.#check(this.#q.module._qz_sandbox_import_state(
            this.address, p, n))));
    }

    /** Replaces the program, keeping bindings and limits and migrating every state; on failure nothing changes. */
    reload(program) {
        this.#run(() => {
            this.#check(this.#q.module._qz_sandbox_reload(this.address, program.address));
            this.#program = program;
            this.#tasks.clear();
            for (const b of this.#bindings) {
                b.signature = program.imports.find((i) => i.name === b.name)?.signature ?? b.signature;
            }
        });
    }

    /** The `@persist(per = Handle)` states of one handle as JSON. */
    exportHandle(handle) {
        return this.#run(() => {
            this.#q.setSlot(this.#q.scratch, BigInt(handle));
            return this.#json(this.#q.module._qzw_export_handle(this.address, this.#q.scratch));
        });
    }

    importHandle(handle, json) {
        this.#run(() => this.#withText(json, (p, n) => {
            this.#q.setSlot(this.#q.scratch, BigInt(handle));
            this.#check(this.#q.module._qzw_import_handle(this.address, this.#q.scratch, p, n));
        }));
    }

    dropHandle(handle) {
        this.#run(() => {
            this.#q.setSlot(this.#q.scratch, BigInt(handle));
            this.#q.module._qzw_drop_handle(this.address, this.#q.scratch);
        });
    }

    /** Binds this sandbox's imports `pkg.fn` to `provider`'s exports; keep the provider open while linked. */
    link(pkg, provider) {
        this.#run(() => {
            const n = this.#q.cString(pkg);
            const status = this.#q.module._qz_sandbox_link(this.address, n, provider.address);
            this.#q.module._free(n);
            if (status !== OK) {
                throw new QuartzError(this.#error());
            }
        });
    }

    get fuelPerCall() {
        this.#q.module._qzw_fuel(this.address, this.#q.scratch);
        return this.#q.view().getBigInt64(this.#q.scratch + 8, true);
    }

    set fuelPerCall(fuel) {
        this.#q.setSlot(this.#q.scratch, BigInt(fuel));
        this.#q.module._qzw_set_fuel(this.address, this.#q.scratch);
    }

    get fuelLeft() {
        this.#q.module._qzw_fuel(this.address, this.#q.scratch);
        return this.#q.view().getBigInt64(this.#q.scratch, true);
    }

    get memoryUsed() {
        return this.#q.module._qz_sandbox_memory_used(this.address) >>> 0;
    }

    get liveTasks() {
        return this.#q.module._qz_sandbox_live_tasks(this.address) >>> 0;
    }

    /** Frees the sandbox and its host functions. */
    close() {
        if (this.#address === 0) {
            return;
        }
        if (this.#active) {
            throw new QuartzError("a sandbox cannot be closed from inside its own call");
        }
        const m = this.#q.module;
        m._qz_sandbox_free(this.#address);
        this.#bindings.forEach((b) => this.#q.unbind(b.id));
        if (this.#args) {
            m._free(this.#args);
        }
        this.#address = 0;
    }

    runHost(binding, call, args, result) {
        const q = this.#q;
        try {
            const sig = binding.signature;
            const values = [];
            let at = args;
            for (const p of sig.params) {
                values.push(this.#values.decodeAt(p, at));
                at += 8 * p.slots;
            }
            values.push({ signature: sig, charge: (units) => {
                q.setSlot(q.scratch + 32, BigInt(units));
                return !!q.module._qzw_call_charge(call, q.scratch + 32);
            } });
            const value = binding.fn(...values);
            if (sig.result.kind !== "void") {
                this.#values.encodeInto(sig.result, value, call, result);
            }
            return true;
        } catch (e) {
            this.#failure = e;
            const message = q.cString(e instanceof Error ? `${e.name}: ${e.message}` : String(e));
            q.module._qz_call_fail(call, message);
            q.module._free(message);
            return false;
        }
    }

    #run(body) {
        if (this.#active) {
            throw new QuartzError("sandbox is already running a call; it cannot be re-entered");
        }
        this.#active = true;
        this.#failure = undefined;
        try {
            return body();
        } finally {
            this.#active = false;
        }
    }

    #function(name) {
        const f = typeof name === "string" ? this.#program.function(name) : name;
        if (f === undefined) {
            throw new RangeError(`no function named ${name}`);
        }
        return f;
    }

    #argv(signature, args) {
        if (args.length !== signature.params.length) {
            throw new TypeError(`expected ${signature.params.length} arguments, got ${args.length}`);
        }
        const slots = signature.params.reduce((n, p) => n + p.slots, 0);
        if (slots > this.#argCapacity) {
            if (this.#args) {
                this.#q.module._free(this.#args);
            }
            this.#argCapacity = Math.max(8, slots);
            this.#args = this.#q.module._malloc(8 * this.#argCapacity);
        }
        let at = this.#args;
        args.forEach((a, i) => {
            this.#values.encodeInto(signature.params[i], a, false, at);
            at += 8 * signature.params[i].slots;
        });
        return slots;
    }

    #outcome() {
        const q = this.#q;
        const kinds = ["done", "wait", "at_enter", "at_leave", "yield"];
        const kind = kinds[q.u32(q.scratch + 16)] ?? "yield";
        const task = q.u32(q.scratch + 20);
        const bits = q.slot(q.scratch + 24);
        let value;
        if (kind === "done") {
            const type = this.#tasks.get(task);
            this.#tasks.delete(task);
            value = type === undefined ? bits : type.slots > 1 ? this.#wideResult(task, type)
                : this.#values.decodeAt(type, q.scratch + 24);
        } else {
            value = bits;
        }
        return { kind, task, value };
    }

    #wideResult(task, type) {
        const m = this.#q.module;
        const cell = m._malloc(8 * type.slots);
        try {
            this.#check(m._qz_sandbox_task_result(this.address, task, cell, type.slots));
            return this.#values.decodeAt(type, cell);
        } finally {
            m._free(cell);
        }
    }

    #withText(text, use) {
        const bytes = encoder.encode(text);
        const p = this.#q.bytesIn(bytes);
        try {
            use(p, bytes.length);
        } finally {
            this.#q.module._free(p);
        }
    }

    #json(ptr) {
        const text = this.#q.take(ptr);
        if (text === null) {
            throw new TrapError(TrapCode[this.#q.module._qz_sandbox_trap_code(this.address)] ?? "panic", this.#error());
        }
        return text;
    }

    #check(status) {
        if (status === OK) {
            return;
        }
        const cause = this.#failure;
        this.#failure = undefined;
        if (status === TRAP) {
            throw new TrapError(TrapCode[this.#q.module._qz_sandbox_trap_code(this.address)] ?? "panic", this.#error(),
                cause);
        }
        throw new QuartzError(this.#error());
    }

    #error() {
        return this.#q.module.UTF8ToString(this.#q.module._qz_sandbox_error(this.address)) || "unknown error";
    }
}
