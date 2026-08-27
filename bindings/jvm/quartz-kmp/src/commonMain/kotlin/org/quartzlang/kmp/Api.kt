package org.quartzlang.kmp

/** A compiled or loaded program, immutable and shareable; sandboxes made from it outlive [close]. */
public interface Program : AutoCloseable {
    /** Warnings the compiler printed; empty for a loaded module. */
    public val diagnostics: String

    /** The host functions it imports, the runtime's own included. */
    public val imports: List<Import>

    /** The .qzb bytes, when compiled with [CompileOptions.emitBinary] or loaded. */
    public fun binary(): ByteArray?

    public fun find(name: String): Export?

    public fun export(name: String): Export = find(name) ?: throw NoSuchElementException("no function named $name")

    public fun findImport(name: String): Import? = imports.firstOrNull { it.name == name }

    /** The .qzi other packages compile against; [manifest] is package.toml text, or null for the compiled one. */
    public fun packageInterface(manifest: String? = null): String
}

/** A function of one program, resolved once so calls skip the lookup. */
public interface Export {
    public val name: String
    public val signature: Signature
}

/** A host import: returns a boxed scalar, String, ByteArray, or anything for void; what it throws becomes a trap. */
public fun interface HostFunction {
    public fun call(call: HostCall): Any?
}

/** A running host function's arguments, checked against the import's types; valid only while it runs. */
public interface HostCall {
    public val signature: Signature
    public val arity: Int get() = signature.arity

    public fun long(index: Int): Long
    public fun int(index: Int): Int = long(index).toInt()
    public fun double(index: Int): Double
    public fun float(index: Int): Float = double(index).toFloat()
    public fun bool(index: Int): Boolean
    public fun codePoint(index: Int): Int
    public fun string(index: Int): String
    public fun bytes(index: Int): ByteArray

    /** Argument [index] boxed by its kind. */
    public fun arg(index: Int): Any?

    /** A list argument with its elements boxed by the element type. */
    public fun list(index: Int): List<Any?>

    /** Charges fuel for native work; false when the budget is gone. */
    public fun charge(units: Long): Boolean
}

/** One result slot of a known type; a str or list result is readable until the next call into its sandbox. */
public interface Value {
    public val type: Type
    public val kind: Kind get() = type.kind

    /** The raw 64 bits of the slot. */
    public val bits: Long

    /** False once a later call may have freed what a str or list result points to. */
    public val isCurrent: Boolean

    public fun asLong(): Long
    public fun asInt(): Int = asLong().toInt()
    public fun asDouble(): Double
    public fun asFloat(): Float
    public fun asBoolean(): Boolean
    public fun asCodePoint(): Int
    public fun asString(): String
    public fun asBytes(): ByteArray

    /** The value boxed by its type: Long, Double, Float, Boolean, Int (char), String, ByteArray, List, LongArray. */
    public fun get(): Any?

    public fun listLength(): Int

    /** A list result with its elements boxed by the element type, nested lists included. */
    public fun asList(): List<Any?>

    /** Every raw slot, as many as `type.slots`: how a struct or tuple result comes back for now. */
    public fun slots(): LongArray
}

/** An isolated instance of a program with its own memory, fuel and state, used by one thread at a time. */
public interface Sandbox : AutoCloseable {
    public val program: Program
    public val isClosed: Boolean

    /** Binds the host import [importName] as the .qzi spells it. */
    public fun bind(importName: String, function: HostFunction)

    /** Checks every import is bound, creates the states and runs their initializers. */
    public fun start()

    public fun call(function: Export, vararg args: Any?): Value
    public fun call(name: String, vararg args: Any?): Value = call(program.export(name), *args)

    /** Starts a task (a `suspend fn`); it runs until it finishes, waits or spends its fuel slice. */
    public fun spawn(function: Export, vararg args: Any?): TaskOutcome
    public fun spawn(name: String, vararg args: Any?): TaskOutcome = spawn(program.export(name), *args)

    /** Resumes a task with what its `wait` produces, checked against the awaited type. */
    public fun resume(task: Int, value: Any?): TaskOutcome
    public fun cancel(task: Int)

    public fun exportState(): String
    public fun importState(json: String)

    /** Replaces the program in place, keeping bindings and limits and migrating every state. */
    public fun reload(next: Program)

    public fun exportHandle(handle: Long): String
    public fun importHandle(handle: Long, json: String)
    public fun dropHandle(handle: Long)

    /** Binds this sandbox's imports `pkg.fn` to [provider]'s exports, run on the provider's budget. */
    public fun link(pkg: String, provider: Sandbox)

    public var fuelPerCall: Long
    public val fuelLeft: Long
    public val memoryUsed: Long
    public val liveTasks: Long
}

/** Entry point: compiles sources and loads modules on whatever runtime this platform has. */
public object Quartz {
    /** Loads the runtime where that is asynchronous (WebAssembly in JavaScript); does nothing on other targets. */
    public suspend fun initialize(): Unit = platformInitialize()

    /** The ABI version of the loaded libquartz. */
    public val abiVersion: Int get() = platformAbiVersion()

    public fun compile(vararg sources: Source, options: CompileOptions = CompileOptions()): Program =
        platformCompile(sources.toList(), options)

    public fun compile(sources: List<Source>, options: CompileOptions = CompileOptions()): Program =
        platformCompile(sources, options)

    /** Loads a .qzb module; with trusted Ed25519 public keys (32 bytes each) its signature is checked too. */
    public fun load(bytes: ByteArray, trustedKeys: List<ByteArray> = emptyList(), requireSignature: Boolean = false):
        Program = platformLoad(bytes, trustedKeys, requireSignature)

    public fun sandbox(program: Program, limits: Limits = Limits.DEFAULT): Sandbox = platformSandbox(program, limits)
}

internal expect suspend fun platformInitialize()

internal expect fun platformAbiVersion(): Int

internal expect fun platformCompile(sources: List<Source>, options: CompileOptions): Program

internal expect fun platformLoad(bytes: ByteArray, trustedKeys: List<ByteArray>, requireSignature: Boolean): Program

internal expect fun platformSandbox(program: Program, limits: Limits): Sandbox
