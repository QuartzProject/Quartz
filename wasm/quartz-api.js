// The embedding API of Quartz for JavaScript, over quartz-capi.js (the C interface built with Emscripten).
// Types and documentation are in quartz-api.d.ts.

export const Kind = Object.freeze(["void", "bool", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64",
    "char", "str", "bytes", "list", "map", "struct", "enum", "nullable", "tuple", "handle", "function", "other"]);
export const TrapCode = Object.freeze(["none", "unreachable", "divide_by_zero", "index_out_of_bounds", "null_unwrap",
    "out_of_fuel", "out_of_memory", "stack_overflow", "invalid_handle", "panic", "host_error"]);
const Tier = { default: 0, interpreter: 1, automatic: 2, native: 3 };
const OK = 0;
const TRAP = 2;
const BITS = { i8: 8, i16: 16, i32: 32, u8: 8, u16: 16, u32: 32 };
const REFERENCES = new Set(["str", "bytes", "list", "map"]);

export class QuartzError extends Error {
    constructor(message, options) {
        super(message, options);
        this.name = "QuartzError";
    }
}

export class CompileError extends QuartzError {
    constructor(diagnostics) {
        super(diagnostics.trim() || "compilation failed");
        this.name = "CompileError";
        this.diagnostics = diagnostics;
    }
}

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
const decoder = new TextDecoder();

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
        this.i32(this.scratch, 0);
        const p = m._qzw_compile(names, texts, lengths, list.length, flags, manifest, this.scratch) >>> 0;
        owned.forEach((ptr) => ptr && m._free(ptr));
        const diagnostics = this.take(this.u32(this.scratch)) ?? "";
        if (p === 0) {
            throw new CompileError(diagnostics);
        }
        return new Program(this, p, diagnostics);
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
        const kind = Kind[this.#m._qz_type_kind(program, id, this.scratch)] ?? "other";
        const type = { kind, slots: this.u32(this.scratch), id };
        cache.set(id, type);
        const element = this.#m._qz_type_elem(program, id);
        if (element !== 0) {
            type.element = this.type(program, element, cache);
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
                    native: !!m._qz_program_import_is_native(p, i), signature: { params, result } });
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

    constructor(quartz, program, limits) {
        this.#q = quartz;
        this.#program = program;
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
                return this.#decodeAt(f.signature.result, out);
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
            const bits = value === undefined || value === null ? 0n : this.#encode(type, value, false);
            this.#q.setSlot(this.#q.scratch, bits);
            this.#check(m._qzw_resume(this.address, task, this.#q.scratch, this.#q.scratch + 16));
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
                values.push(this.#decodeAt(p, at));
                at += 8 * p.slots;
            }
            values.push({ signature: sig, charge: (units) => {
                q.setSlot(q.scratch + 32, BigInt(units));
                return !!q.module._qzw_call_charge(call, q.scratch + 32);
            } });
            const value = binding.fn(...values);
            if (sig.result.kind !== "void") {
                q.setSlot(result, this.#encode(sig.result, value, call));
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
        if (args.length > this.#argCapacity) {
            if (this.#args) {
                this.#q.module._free(this.#args);
            }
            this.#argCapacity = Math.max(8, args.length);
            this.#args = this.#q.module._malloc(8 * this.#argCapacity);
        }
        args.forEach((a, i) => this.#q.setSlot(this.#args + 8 * i, this.#encode(signature.params[i], a, false)));
        return args.length;
    }

    /** One slot the program reads as `type`; `call` is the host call being answered, or false outside one. */
    #encode(type, value, call) {
        const { kind } = type;
        if (type.slots !== 1) {
            throw new TypeError(`a ${kind} value cannot cross the C interface yet`);
        }
        const fail = () => new TypeError(`${typeof value} ${String(value)} cannot be passed as ${kind}`);
        switch (kind) {
        case "bool":
            if (typeof value !== "boolean") throw fail();
            return value ? 1n : 0n;
        case "i8": case "i16": case "i32": case "u8": case "u16": case "u32": case "i64": case "u64": case "handle": {
            let n;
            if (typeof value === "bigint") {
                n = value;
            } else if (Number.isSafeInteger(value)) {
                n = BigInt(value);
            } else {
                throw fail();
            }
            const bits = BITS[kind];
            const signed = kind.startsWith("i");
            const width = bits ?? 64;
            const fits = signed ? BigInt.asIntN(width, n) === n : n >= 0n && BigInt.asUintN(width, n) === n;
            if (!fits) throw new RangeError(`${n} does not fit in ${kind}`);
            return BigInt.asUintN(64, n);
        }
        case "f64":
            if (typeof value !== "number") throw fail();
            return new BigUint64Array(new Float64Array([value]).buffer)[0];
        case "f32":
            if (typeof value !== "number") throw fail();
            return BigInt(new Uint32Array(new Float32Array([value]).buffer)[0]);
        case "char": {
            const cp = typeof value === "string" && [...value].length === 1 ? value.codePointAt(0) : value;
            if (!Number.isInteger(cp) || cp < 0 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) throw fail();
            return BigInt(cp);
        }
        case "str":
            if (typeof value !== "string") throw fail();
            return this.#newStr(encoder.encode(value), call);
        case "bytes":
            if (!(value instanceof Uint8Array)) throw fail();
            return this.#newStr(value, call);
        case "list":
            if (!Array.isArray(value)) throw fail();
            return this.#newList(type, value, call);
        case "nullable":
            if (value === null || value === undefined) {
                if (!REFERENCES.has(type.element?.kind)) throw fail();
                return 0n;
            }
            if (!REFERENCES.has(type.element?.kind)) {
                throw new TypeError(`a ${kind} value cannot cross the C interface yet`);
            }
            return this.#encode(type.element, value, call);
        default:
            throw new TypeError(`a ${kind} value cannot cross the C interface yet`);
        }
    }

    #newStr(bytes, call) {
        const q = this.#q;
        const data = q.bytesIn(bytes);
        const out = q.scratch + 40;
        if (call) {
            q.module._qzw_call_str(call, data, bytes.length, out);
        } else {
            q.module._qzw_sandbox_str(this.address, data, bytes.length, out);
        }
        q.module._free(data);
        const bits = q.slot(out);
        if (bits === 0n) {
            throw new TrapError("out_of_memory", "out of memory passing a string");
        }
        return bits;
    }

    #newList(type, values, call) {
        const q = this.#q;
        const m = q.module;
        const cell = m._malloc(16);
        try {
            m._qzw_sandbox_list(this.address, type.id, cell);
            if (q.slot(cell) === 0n) {
                throw new TrapError("out_of_memory", "cannot make a list");
            }
            for (const v of values) {
                q.setSlot(cell + 8, this.#encode(type.element, v, call));
                if (!m._qzw_list_push(this.address, cell, cell + 8, 1)) {
                    throw new TrapError("out_of_memory", "cannot grow the list");
                }
            }
            return q.slot(cell);
        } finally {
            m._free(cell);
        }
    }

    #decodeAt(type, ptr) {
        if (type.slots > 1) {
            const raw = [];
            for (let i = 0; i < type.slots; ++i) {
                raw.push(this.#q.slot(ptr + 8 * i));
            }
            return raw;
        }
        return type.kind === "void" ? undefined : this.#decode(type, this.#q.slot(ptr));
    }

    #decode(type, bits) {
        const q = this.#q;
        switch (type.kind) {
        case "void": return undefined;
        case "bool": return bits !== 0n;
        case "i8": case "i16": case "i32": return Number(BigInt.asIntN(BITS[type.kind], bits));
        case "u8": case "u16": case "u32": return Number(BigInt.asUintN(BITS[type.kind], bits));
        case "i64": return BigInt.asIntN(64, bits);
        case "u64": case "handle": return BigInt.asUintN(64, bits);
        case "f64": return new Float64Array(new BigUint64Array([bits]).buffer)[0];
        case "f32": return new Float32Array(new Uint32Array([Number(BigInt.asUintN(32, bits))]).buffer)[0];
        case "char": return String.fromCodePoint(Number(bits));
        case "str": return decoder.decode(q.strBytes(bits));
        case "bytes": return q.strBytes(bits);
        case "list": return this.#readList(type, bits);
        case "nullable":
            if (type.slots === 1 && REFERENCES.has(type.element?.kind)) {
                return bits === 0n ? null : this.#decode(type.element, bits);
            }
            return bits;
        default: return bits;
        }
    }

    #readList(type, bits) {
        const q = this.#q;
        const m = q.module;
        const width = Math.max(1, type.element.slots);
        const cell = m._malloc(8 + 8 * width);
        try {
            q.setSlot(cell, bits);
            const out = [];
            const n = m._qzw_list_length(cell) >>> 0;
            for (let i = 0; i < n; ++i) {
                if (!m._qzw_list_get(cell, i, cell + 8, width)) {
                    throw new QuartzError(`cannot read element ${i}`);
                }
                out.push(this.#decodeAt(type.element, cell + 8));
            }
            return out;
        } finally {
            m._free(cell);
        }
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
            value = type === undefined ? bits : this.#decode(type, bits);
        } else {
            value = bits;
        }
        return { kind, task, value };
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
