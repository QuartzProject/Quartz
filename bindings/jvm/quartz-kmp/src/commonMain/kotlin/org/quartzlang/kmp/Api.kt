package org.quartzlang.kmp

/** A compiled or loaded program, immutable and shareable; sandboxes made from it outlive [close]. */
public interface Program : AutoCloseable {
    /** The warnings and notes of a compiled program; empty for a loaded module. */
    public val diagnostics: List<Diagnostic>

    /** [diagnostics] rendered as the compiler prints them. */
    public val diagnosticsText: String

    /** The functions the program offers the host, in declaration order. */
    public val exports: List<Export>

    /** The host functions it imports, the runtime's own included. */
    public val imports: List<Import>

    /** The .qzb bytes, when compiled with [CompileOptions.emitBinary] or loaded. */
    public fun binary(): ByteArray?

    /**
     * The function called [name], with its package or, when only one function has it, without.
     *
     * @throws IllegalArgumentException when several functions share [name] without a package
     */
    public fun find(name: String): Export?

    public fun export(name: String): Export = find(name) ?: throw NoSuchElementException("no function named $name")

    /**
     * The import called [name] with its package, or without it when only one import has that name.
     *
     * @throws IllegalArgumentException when several imports share [name] without a package
     */
    public fun findImport(name: String): Import?

    /** The .qzi other packages compile against; [manifest] is package.toml text, or null for the compiled one. */
    public fun packageInterface(manifest: String? = null): String
}

/** A function of one program, resolved once so calls skip the lookup. */
public interface Export {
    /** The name with its package (`org.example.greet`, `org.example.Shop.buy`). */
    public val name: String
    public val signature: Signature

    /** The package (`org.example`); empty for a program without one. */
    public val packageName: String

    /** The name without its package (`greet`, `Shop.buy`). */
    public val simpleName: String get() = simpleNameOf(name, packageName)

    /** One per parameter of [signature]; empty strings for a function that is not exported. */
    public val parameterNames: List<String>
    public val visibility: Visibility

    /** A `suspend fn`, which runs as a task with [Sandbox.spawn]. */
    public val isSuspend: Boolean

    /** Offered to the host ([Program.exports]). */
    public val isExported: Boolean

    /** Its attributes as written (`@on`, `@command`...), in source order. */
    public val attributes: List<Attribute>

    /** The first attribute called [name] (without the `@`), or null. */
    public fun attribute(name: String): Attribute? = attributes.firstOrNull { it.name == name }
}

/** A host import: returns what [Sandbox.call] accepts for its result type; what it throws becomes a trap. */
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

    /** Argument [index] boxed by its type, as [Value.get] does. */
    public fun arg(index: Int): Any?

    /** A list argument with its elements boxed by the element type. */
    public fun list(index: Int): List<Any?>

    /** A map argument in insertion order. */
    public fun map(index: Int): Map<Any?, Any?>

    /** Charges fuel for native work; false when the budget is gone. */
    public fun charge(units: Long): Boolean
}

/** A result of a known type; what points into the sandbox (str, lists, maps) is readable until its next call. */
public interface Value {
    public val type: Type
    public val kind: Kind get() = type.kind

    /** The raw 64 bits of the first slot. */
    public val bits: Long

    /** False once a later call may have freed what a str or list result points to. */
    public val isCurrent: Boolean

    /** True for void and an empty nullable. */
    public val isNull: Boolean get() = kind == Kind.VOID || (kind == Kind.NULLABLE && bits == 0L)

    public fun asLong(): Long
    public fun asInt(): Int = asLong().toInt()
    public fun asDouble(): Double
    public fun asFloat(): Float
    public fun asBoolean(): Boolean
    public fun asCodePoint(): Int
    public fun asString(): String
    public fun asBytes(): ByteArray

    /**
     * The value boxed by its type: Long (integers and handles), Double, Float, Boolean, Int (char), String,
     * ByteArray, List (lists and tuples), Map (in insertion order), [Struct], [Variant], [Result], or null for void
     * and an empty nullable.
     */
    public fun get(): Any?

    public fun listLength(): Int

    /** A list result with its elements boxed by the element type, nested lists included. */
    public fun asList(): List<Any?>

    public fun asStruct(): Struct = checkKind(Kind.STRUCT).get() as Struct

    /** An enum result; a `Result` reads as variant `Ok` or `Err` too. */
    public fun asVariant(): Variant = when (val v = checkKind(Kind.ENUM).get()) {
        is Result.Ok<*> -> Variant("Ok", listOf(v.value))
        is Result.Err<*> -> Variant("Err", listOf(v.error))
        else -> v as Variant
    }

    @Suppress("UNCHECKED_CAST")
    public fun asResult(): Result<Any?, Any?> {
        check(type.isResult) { "the value is $type, asResult does not apply" }
        return get() as Result<Any?, Any?>
    }

    @Suppress("UNCHECKED_CAST")
    public fun asTuple(): List<Any?> = checkKind(Kind.TUPLE).get() as List<Any?>

    @Suppress("UNCHECKED_CAST")
    public fun asMap(): Map<Any?, Any?> = checkKind(Kind.MAP).get() as Map<Any?, Any?>

    /** Every raw slot, as many as `type.slots`. */
    public fun slots(): LongArray
}

private fun Value.checkKind(kind: Kind): Value {
    check(this.kind == kind) { "the value is $type, not a ${kind.name.lowercase()}" }
    return this
}

/** An isolated instance of a program with its own memory, fuel and state, used by one thread at a time. */
public interface Sandbox : AutoCloseable {
    public val program: Program
    public val isClosed: Boolean

    /** Binds the host import [importName] as the .qzi spells it. */
    public fun bind(importName: String, function: HostFunction)

    /** Checks every import is bound, creates the states and runs their initializers. */
    public fun start()

    /**
     * Calls an exported function. Arguments are boxed scalars, String, ByteArray, List or Array, Map (maps, or a
     * struct by field name), [Struct], Pair or Triple (tuples), [Variant], an enum constant, [Result], null for an
     * empty nullable, a [Value] of this sandbox, or a `@Serializable` value passed through [quartzValue].
     */
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

    /** The warning rule ids [CompileOptions.withRule] accepts (`unused-variable`...). */
    public val warningRules: List<String> get() = platformWarningRules()

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

internal expect fun platformWarningRules(): List<String>

internal expect fun platformCompile(sources: List<Source>, options: CompileOptions): Program

internal expect fun platformLoad(bytes: ByteArray, trustedKeys: List<ByteArray>, requireSignature: Boolean): Program

internal expect fun platformSandbox(program: Program, limits: Limits): Sandbox
