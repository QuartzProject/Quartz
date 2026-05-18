// The embedding API of Quartz for JavaScript and TypeScript (quartz-api.js over quartz-capi.wasm).

/** A type's kind as the C interface reports it. */
export type Kind = "void" | "bool" | "i8" | "i16" | "i32" | "i64" | "u8" | "u16" | "u32" | "u64" | "f32" | "f64" |
    "char" | "str" | "bytes" | "list" | "map" | "struct" | "enum" | "nullable" | "tuple" | "handle" | "function" |
    "other";

/** Why a program trapped. */
export type TrapCode = "none" | "unreachable" | "divide_by_zero" | "index_out_of_bounds" | "null_unwrap" |
    "out_of_fuel" | "out_of_memory" | "stack_overflow" | "invalid_handle" | "panic" | "host_error";

export declare const Kind: readonly Kind[];
export declare const TrapCode: readonly TrapCode[];

/** A parameter or result type: its kind, the slots it takes, its id, and what it is made of. */
export interface Type {
    kind: Kind;
    slots: number;
    id: number;
    /** The element of a list or nullable, the value of a map. */
    element?: Type;
    /** The key of a map. */
    key?: Type;
    /** A nullable of a reference type is one slot, null when 0. */
    pointer?: boolean;
    /** Fields of a struct (named) or elements of a tuple, with their slot offsets. */
    fields?: { name?: string; offset: number; type: Type }[];
    /** Variants of an enum; slot 0 holds the index and payloads start at slot 1. */
    variants?: { name: string; payload: { offset: number; type: Type }[] }[];
}

export interface Signature {
    params: Type[];
    result: Type;
}

export interface Export {
    name: string;
    index: number;
    signature: Signature;
}

/** A host function a program imports; the runtime binds the `native` ones (the standard library) itself. */
export interface Import {
    name: string;
    index: number;
    native: boolean;
    signature: Signature;
}

// bool: boolean; i8..u32, f32, f64: number; i64, u64, handle: bigint; char, str: string; bytes: Uint8Array; list and
// tuple: array; map: Map in insertion order; struct: object by field name; enum: { variant, values } (a variant without
// payload may be passed as its name); nullable: null or the value.
export type QuartzValue = undefined | null | boolean | number | bigint | string | Uint8Array | QuartzValue[] |
    Map<QuartzValue, QuartzValue> | { variant: string; values: QuartzValue[] } | { [field: string]: QuartzValue };

/** What a running host function gets after its arguments. */
export interface HostCall {
    signature: Signature;
    /** Charges fuel for native work; false when the budget is gone (the call then traps). */
    charge(units: number | bigint): boolean;
}

export type HostFunction = (...args: any[]) => QuartzValue;

export interface Limits {
    /** Memory quota in bytes; 0 or absent keeps the default (64 MiB). */
    memoryBytes?: number;
    /** Fuel per call and per task slice. */
    fuelPerCall?: number | bigint;
    maxDepth?: number;
    /** Only the interpreter runs in WebAssembly, so the tier changes nothing here. */
    tier?: "default" | "interpreter" | "automatic" | "native";
}

export interface CompileOptions {
    /** Also produce the portable .qzb module (Program.binary()). */
    emitBinary?: boolean;
    /** Keep source lines for trap messages (default true). */
    keepLines?: boolean;
    /** package.toml text. */
    manifest?: string;
}

export interface TaskOutcome {
    kind: "done" | "wait" | "at_enter" | "at_leave" | "yield";
    task: number;
    /** done: the result (its raw slot when it is wider than one); otherwise the awaited handle. */
    value: QuartzValue;
}

export declare class QuartzError extends Error {}

export declare class CompileError extends QuartzError {
    /** Every error and warning as the compiler printed them. */
    diagnostics: string;
}

/** The program trapped; the sandbox stays usable. When a host function threw, that is the cause. */
export declare class TrapError extends QuartzError {
    code: TrapCode;
}

export interface Program {
    /** Warnings the compiler printed; empty for a loaded module. */
    readonly diagnostics: string;
    readonly imports: Import[];
    function(name: string): Export | undefined;
    binary(): Uint8Array | null;
    /** The .qzi other packages compile against; `manifest` is package.toml text. */
    packageInterface(manifest?: string): string;
    /** Frees the program; sandboxes made from it keep working. */
    close(): void;
}

/** An isolated instance of a program with its own memory, fuel and state. Close it when done. */
export interface Sandbox {
    readonly program: Program;
    /** Binds a host import; `fn` gets the decoded arguments and, last, a HostCall. What it throws becomes a trap. */
    bind(name: string, fn: HostFunction): void;
    /** Checks every import is bound, creates the states and runs their initializers. */
    start(): void;
    /** Calls an exported function, checking the arguments against its signature; a trap throws TrapError. */
    call(name: string | Export, ...args: QuartzValue[]): QuartzValue;
    spawn(name: string | Export, ...args: QuartzValue[]): TaskOutcome;
    /** Resumes a task with what its `wait` produces, checked against the awaited type. */
    resume(task: number, value?: QuartzValue): TaskOutcome;
    cancel(task: number): void;
    exportState(): string;
    importState(json: string): void;
    /** Replaces the program in place, keeping bindings and limits and migrating every state. */
    reload(program: Program): void;
    exportHandle(handle: number | bigint): string;
    importHandle(handle: number | bigint, json: string): void;
    dropHandle(handle: number | bigint): void;
    /** Binds this sandbox's imports `pkg.fn` to `provider`'s exports; keep the provider open while linked. */
    link(pkg: string, provider: Sandbox): void;
    fuelPerCall: bigint;
    readonly fuelLeft: bigint;
    readonly memoryUsed: number;
    readonly liveTasks: number;
    close(): void;
}

export interface Quartz {
    readonly abiVersion: number;
    /** Compiles sources given as `{ name: text }` or `[{ name, text }]`; a name ending in .qzi is a host interface. */
    compile(sources: Record<string, string> | { name: string; text: string }[], options?: CompileOptions): Program;
    /** Loads a .qzb module, checking its hash and, with trusted Ed25519 public keys, its signature. */
    load(bytes: Uint8Array, options?: { trustedKeys?: Uint8Array[]; requireSignature?: boolean }): Program;
    sandbox(program: Program, limits?: Limits): Sandbox;
}

/** Instantiates quartz-capi.wasm. In Node it is found next to this file; in a browser pass `createModule`. */
export declare function loadQuartz(options?: {
    createModule?: (settings: object) => Promise<object>;
    locateFile?: (path: string, prefix: string) => string;
}): Promise<Quartz>;
