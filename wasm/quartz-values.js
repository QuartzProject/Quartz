// Converts between JavaScript values and Quartz values laid out in slots of WebAssembly memory, for one sandbox.
// quartz-api.d.ts describes the mapping (QuartzValue).

import { QuartzError, TrapError } from "./quartz-api.js";

const BITS = { i8: 8, i16: 16, i32: 32, u8: 8, u16: 16, u32: 32 };
const encoder = new TextEncoder();
const decoder = new TextDecoder();

export class Values {
    #q;
    #sandbox;

    /** `quartz` gives memory access and the module, `sandbox` its address (it may close later). */
    constructor(quartz, sandbox) {
        this.#q = quartz;
        this.#sandbox = sandbox;
    }

    /** Writes `value` as `type` into the slots at `ptr`; `call` is the host call being answered, or false. */
    encodeInto(type, value, call, ptr) {
        const q = this.#q;
        switch (type.kind) {
        case "struct":
        case "tuple": {
            const tuple = type.kind === "tuple";
            if (tuple ? !Array.isArray(value) || value.length !== type.fields.length
                : value === null || typeof value !== "object") {
                throw this.#mismatch(type, value);
            }
            type.fields.forEach((f, i) => this.encodeInto(f.type, tuple ? value[i] : value[f.name], call,
                ptr + 8 * f.offset));
            return;
        }
        case "enum": {
            const name = typeof value === "string" ? value : value?.variant;
            const index = type.variants.findIndex((v) => v.name === name);
            const values = typeof value === "string" ? [] : value?.values ?? [];
            if (index < 0 || !Array.isArray(values) || values.length !== type.variants[index].payload.length) {
                throw this.#mismatch(type, value);
            }
            this.#zero(ptr, type.slots);
            q.setSlot(ptr, BigInt(index));
            type.variants[index].payload.forEach((p, i) => this.encodeInto(p.type, values[i], call,
                ptr + 8 * p.offset));
            return;
        }
        case "nullable":
            if (type.pointer) {
                if (value === null || value === undefined) {
                    q.setSlot(ptr, 0n);
                } else {
                    this.encodeInto(type.element, value, call, ptr);
                }
                return;
            }
            this.#zero(ptr, type.slots);
            if (value !== null && value !== undefined) {
                q.setSlot(ptr, 1n);
                this.encodeInto(type.element, value, call, ptr + 8);
            }
            return;
        default:
            q.setSlot(ptr, this.#scalar(type, value, call));
        }
    }

    /** Reads a value of `type` from the slots at `ptr`. */
    decodeAt(type, ptr) {
        const q = this.#q;
        switch (type.kind) {
        case "void":
            return undefined;
        case "struct":
            return Object.fromEntries(type.fields.map((f) => [f.name, this.decodeAt(f.type, ptr + 8 * f.offset)]));
        case "tuple":
            return type.fields.map((f) => this.decodeAt(f.type, ptr + 8 * f.offset));
        case "enum": {
            const variant = type.variants[Number(q.slot(ptr))];
            if (variant === undefined) {
                throw new QuartzError(`enum value with an unknown variant ${q.slot(ptr)}`);
            }
            const values = variant.payload.map((p) => this.decodeAt(p.type, ptr + 8 * p.offset));
            return { variant: variant.name, values };
        }
        case "nullable":
            if (type.pointer) {
                return q.slot(ptr) === 0n ? null : this.decodeAt(type.element, ptr);
            }
            return q.slot(ptr) === 0n ? null : this.decodeAt(type.element, ptr + 8);
        default:
            return this.#decode(type, q.slot(ptr));
        }
    }

    #scalar(type, value, call) {
        const { kind } = type;
        const fail = () => this.#mismatch(type, value);
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
            const width = BITS[kind] ?? 64;
            const signed = kind.startsWith("i");
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
            return this.#newStr(value, call, true);
        case "list":
            if (!Array.isArray(value)) throw fail();
            return this.#newList(type, value, call);
        case "map":
            if (!(value instanceof Map)) throw fail();
            return this.#newMap(type, value, call);
        default:
            throw new TypeError(`a ${kind} value cannot cross the C interface`);
        }
    }

    #mismatch(type, value) {
        return new TypeError(`${typeof value} ${String(value)} cannot be passed as ${type.kind}`);
    }

    #zero(ptr, slots) {
        for (let i = 0; i < slots; ++i) {
            this.#q.setSlot(ptr + 8 * i, 0n);
        }
    }

    #newStr(bytes, call, raw = false) {
        const q = this.#q;
        const m = q.module;
        const data = q.bytesIn(bytes);
        const out = q.scratch + 40;
        if (call) {
            (raw ? m._qz_call_bytes_into : m._qzw_call_str)(call, data, bytes.length, out);
        } else {
            (raw ? m._qz_sandbox_bytes_into : m._qzw_sandbox_str)(this.#sandbox.address, data, bytes.length, out);
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
        const width = Math.max(1, type.element.slots);
        const cell = m._malloc(8 + 8 * width);
        try {
            m._qzw_sandbox_list(this.#sandbox.address, type.id, cell);
            if (q.slot(cell) === 0n) {
                throw new TrapError("out_of_memory", "cannot make a list");
            }
            for (const v of values) {
                this.encodeInto(type.element, v, call, cell + 8);
                if (!m._qzw_list_push(this.#sandbox.address, cell, cell + 8, width)) {
                    throw new TrapError("out_of_memory", "cannot grow the list");
                }
            }
            return q.slot(cell);
        } finally {
            m._free(cell);
        }
    }

    #newMap(type, entries, call) {
        const q = this.#q;
        const m = q.module;
        const keyAt = 8;
        const valueAt = 8 + 8 * Math.max(1, type.key.slots);
        const cell = m._malloc(valueAt + 8 * Math.max(1, type.element.slots));
        try {
            m._qz_sandbox_map_into(this.#sandbox.address, type.id, cell);
            if (q.slot(cell) === 0n) {
                throw new TrapError("out_of_memory", "cannot make a map");
            }
            for (const [k, v] of entries) {
                this.encodeInto(type.key, k, call, cell + keyAt);
                this.encodeInto(type.element, v, call, cell + valueAt);
                if (!m._qz_map_set_at(this.#sandbox.address, cell, cell + keyAt, cell + valueAt)) {
                    throw new TrapError("out_of_memory", "cannot grow the map");
                }
            }
            return q.slot(cell);
        } finally {
            m._free(cell);
        }
    }

    #decode(type, bits) {
        const q = this.#q;
        switch (type.kind) {
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
        case "map": return this.#readMap(type, bits);
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
                out.push(this.decodeAt(type.element, cell + 8));
            }
            return out;
        } finally {
            m._free(cell);
        }
    }

    #readMap(type, bits) {
        const q = this.#q;
        const m = q.module;
        const keyAt = 8;
        const valueAt = 8 + 8 * Math.max(1, type.key.slots);
        const cell = m._malloc(valueAt + 8 * Math.max(1, type.element.slots));
        try {
            q.setSlot(cell, bits);
            const out = new Map();
            const n = m._qz_map_length_at(cell) >>> 0;
            for (let i = 0; i < n; ++i) {
                if (!m._qz_map_entry_at(this.#sandbox.address, cell, i, cell + keyAt, cell + valueAt)) {
                    throw new QuartzError(`cannot read entry ${i}`);
                }
                out.set(this.decodeAt(type.key, cell + keyAt), this.decodeAt(type.element, cell + valueAt));
            }
            return out;
        } finally {
            m._free(cell);
        }
    }
}
