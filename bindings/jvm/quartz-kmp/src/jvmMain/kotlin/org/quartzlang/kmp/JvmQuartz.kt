package org.quartzlang.kmp

import java.util.IdentityHashMap
import java.util.concurrent.ConcurrentHashMap
import org.quartzlang.jvm.CompileException as JCompileException
import org.quartzlang.jvm.Export as JExport
import org.quartzlang.jvm.Kind as JKind
import org.quartzlang.jvm.Limits as JLimits
import org.quartzlang.jvm.Program as JProgram
import org.quartzlang.jvm.Quartz as JQuartz
import org.quartzlang.jvm.QuartzException as JQuartzException
import org.quartzlang.jvm.Result as JResult
import org.quartzlang.jvm.Sandbox as JSandbox
import org.quartzlang.jvm.Signature as JSignature
import org.quartzlang.jvm.Struct as JStruct
import org.quartzlang.jvm.TaskOutcome as JTaskOutcome
import org.quartzlang.jvm.TrapException as JTrapException
import org.quartzlang.jvm.Value as JValue
import org.quartzlang.jvm.Variant as JVariant

internal actual suspend fun platformInitialize() {}

internal actual fun platformAbiVersion(): Int = JQuartz.abiVersion()

internal actual fun platformWarningRules(): List<String> = JQuartz.warningRules()

internal actual fun platformCompile(sources: List<Source>, options: CompileOptions): Program = translated {
    val rules = options.rules.entries.associateTo(LinkedHashMap()) {
        it.key to org.quartzlang.jvm.WarningLevel.entries[it.value.ordinal]
    }
    val opts = org.quartzlang.jvm.CompileOptions(options.emitBinary, options.keepLines, options.manifest, rules)
    JvmProgram(JQuartz.compile(sources.map { org.quartzlang.jvm.Source(it.name, it.text) }, opts))
}

internal actual fun platformLoad(bytes: ByteArray, trustedKeys: List<ByteArray>, requireSignature: Boolean): Program =
    translated { JvmProgram(JProgram.load(bytes, trustedKeys, requireSignature)) }

internal actual fun platformDeflate(data: ByteArray, level: Int, framing: Framing): ByteArray =
    translated { JQuartz.deflate(data, level, org.quartzlang.jvm.Framing.entries[framing.ordinal]) }

internal actual fun platformInflate(data: ByteArray, framing: Framing, maxLength: Int): ByteArray =
    translated { JQuartz.inflate(data, org.quartzlang.jvm.Framing.entries[framing.ordinal], maxLength.toLong()) }

internal actual fun platformCrc32(crc: Long, data: ByteArray): Long = JQuartz.crc32(crc, data)

internal actual fun platformSandbox(program: Program, limits: Limits): Sandbox = translated {
    val tier = JLimits.Tier.entries[limits.tier.ordinal]
    val p = jvm(program)
    JvmSandbox(JSandbox(p.java, JLimits(limits.memoryBytes, limits.fuelPerCall, limits.maxDepth, tier)), p)
}

private fun jvm(program: Program): JvmProgram =
    program as? JvmProgram ?: throw IllegalArgumentException("a program of another runtime")

/** Maps the Java bindings' exceptions onto the common ones, keeping host failures as the cause. */
internal inline fun <R> translated(block: () -> R): R = try {
    block()
} catch (e: JCompileException) {
    throw CompileException(e.text(), e.diagnostics().map(::diagnosticOf))
} catch (e: JTrapException) {
    throw TrapException(TrapCode.entries[e.code().ordinal], e.message, e.cause)
} catch (e: JQuartzException) {
    throw QuartzException(e.message, e.cause)
}

internal fun kindOf(kind: JKind): Kind = Kind.entries[kind.ordinal]

internal fun diagnosticOf(d: org.quartzlang.jvm.Diagnostic): Diagnostic = Diagnostic(
    Diagnostic.Severity.entries[d.severity().ordinal], d.rule(), d.file(), d.line(), d.column(), d.endLine(),
    d.endColumn(), d.message(),
)

private fun importOf(i: org.quartzlang.jvm.Import): Import =
    Import(i.name(), signatureOf(i.signature()), i.builtIn(), i.packageName(), i.required())

internal fun typeOf(t: org.quartzlang.jvm.Type): Type = typeOf(t, IdentityHashMap())

private fun typeOf(t: org.quartzlang.jvm.Type, seen: IdentityHashMap<org.quartzlang.jvm.Type, Type>): Type {
    seen[t]?.let { return it }
    val out = Type(kindOf(t.kind()), t.slots(), t.id())
    seen[t] = out
    fun field(f: org.quartzlang.jvm.Type.Field) =
        Type.Field(f.name(), typeOf(f.type(), seen), f.offset(), f.box()?.let { typeOf(it, seen) }, f.doc())
    out.name = t.name()
    out.packageName = t.packageName()
    out.doc = t.doc()
    out.complete(
        t.element()?.let { typeOf(it, seen) },
        t.key()?.let { typeOf(it, seen) },
        t.fields().map(::field),
        t.cases().map { c -> Type.Case(c.name(), c.index(), c.payload().map(::field), c.doc()) },
        t.isPointerNullable,
    )
    return out
}

/** A value of the Java bindings as the common API gives it. */
internal fun fromJava(value: Any?): Any? = when (value) {
    is JStruct -> Struct(value.names(), (0 until value.size()).map { fromJava(value.get(it)) }, value.type().name())
    is JVariant -> Variant(value.name(), value.values().map(::fromJava))
    is JResult.Ok<*, *> -> Result.Ok(fromJava(value.value()))
    is JResult.Err<*, *> -> Result.Err(fromJava(value.error()))
    is List<*> -> value.map(::fromJava)
    is Map<*, *> -> value.entries.associateTo(LinkedHashMap()) { fromJava(it.key) to fromJava(it.value) }
    else -> value
}

/** A value of the common API as the Java bindings take it; other classes are refused here as on the C targets. */
internal fun toJava(value: Any?): Any? = when (value) {
    null, is Boolean, is Number, is Char, is String, is ByteArray, is Enum<*> -> value
    is JvmValue -> value.java
    is LossyText -> org.quartzlang.jvm.LossyText(value.raw)
    is Unit -> null
    is Struct -> JStruct.of(value.typeName, value.names.zip(value.values.map(::toJava)).toMap(LinkedHashMap()))
    is Variant -> JVariant(value.name, value.values.map(::toJava))
    is Result.Ok<*> -> JResult.ok<Any?, Any?>(toJava(value.value))
    is Result.Err<*> -> JResult.err<Any?, Any?>(toJava(value.error))
    is Pair<*, *> -> listOf(toJava(value.first), toJava(value.second))
    is Triple<*, *, *> -> listOf(toJava(value.first), toJava(value.second), toJava(value.third))
    is List<*> -> value.map(::toJava)
    is Array<*> -> value.map(::toJava)
    is Map<*, *> -> value.entries.associateTo(LinkedHashMap()) { toJava(it.key) to toJava(it.value) }
    else -> throw IllegalArgumentException(
        "a ${value::class.simpleName} cannot be passed: pass a Struct, Map, List, Variant or Result, " +
            "or a @Serializable value through quartzValue()",
    )
}

internal fun signatureOf(s: JSignature): Signature = Signature(s.params().map(::typeOf), typeOf(s.result()))

internal class JvmProgram(val java: JProgram) : Program {
    private val functions = ConcurrentHashMap<JExport, JvmExport>()

    override val diagnostics: List<Diagnostic> by lazy { java.diagnostics().map(::diagnosticOf) }
    override val diagnosticsText: String get() = java.diagnosticsText()
    override val imports: List<Import> by lazy { translated { java.imports().map(::importOf) } }
    override val exports: List<Export> by lazy { translated { java.exports().map(::wrap) } }

    /** The common view of a Java export, one per function. */
    fun wrap(e: JExport): JvmExport = functions.getOrPut(e) { JvmExport(this, e) }

    override fun binary(): ByteArray? = translated { java.binary().orElse(null) }

    override fun find(name: String): Export? = translated { java.find(name).orElse(null)?.let(::wrap) }

    override fun packageInterface(manifest: String?): String = translated { java.packageInterface(manifest) }

    override fun findImport(name: String): Import? = translated {
        java.findImport(name).orElse(null)?.let(::importOf)
    }

    override fun close(): Unit = java.close()
}

internal class JvmExport(val program: JvmProgram, val java: JExport) : Export {
    override val name: String get() = java.name()
    override val signature: Signature = signatureOf(java.signature())
    override val packageName: String get() = java.packageName()
    override val parameterNames: List<String> get() = java.parameterNames()
    override val visibility: Visibility get() = Visibility.entries[java.visibility().ordinal]
    override val isSuspend: Boolean get() = java.isSuspend
    override val isExported: Boolean get() = java.isExported
    override val doc: String get() = java.doc()
    override val attributes: List<Attribute> by lazy {
        java.attributes().map { a ->
            val args = a.arguments().map {
                Attribute.Argument(it.key(), Attribute.ArgumentKind.entries[it.kind().ordinal], it.value())
            }
            Attribute(a.name(), args, a.filter()?.let(program::wrap))
        }
    }
}

internal class JvmValue(val java: JValue) : Value {
    override val type: Type = typeOf(java.type())
    override val bits: Long get() = java.bits()
    override val isCurrent: Boolean get() = java.isCurrent
    override fun asLong(): Long = java.asLong()
    override fun asDouble(): Double = java.asDouble()
    override fun asFloat(): Float = java.asFloat()
    override fun asBoolean(): Boolean = java.asBoolean()
    override fun asCodePoint(): Int = java.asCodePoint()
    override fun asString(): String = java.asString()
    override fun asBytes(): ByteArray = java.asBytes()
    override fun get(): Any? = translated { fromJava(java.get()) }
    override fun listLength(): Int = java.listLength()
    @Suppress("UNCHECKED_CAST")
    override fun asList(): List<Any?> = translated { fromJava(java.asList()) as List<Any?> }
    override fun slots(): LongArray = java.slots()
}

internal class JvmHostCall(val java: org.quartzlang.jvm.HostCall) : HostCall {
    override val signature: Signature get() = signatureOf(java.signature())
    override val arity: Int get() = java.arity()
    override fun long(index: Int): Long = java.longArg(index)
    override fun double(index: Int): Double = java.doubleArg(index)
    override fun bool(index: Int): Boolean = java.boolArg(index)
    override fun codePoint(index: Int): Int = java.codePointArg(index)
    override fun string(index: Int): String = java.stringArg(index)
    override fun bytes(index: Int): ByteArray = java.bytesArg(index)
    override fun arg(index: Int): Any? = translated { fromJava(java.arg(index)) }

    @Suppress("UNCHECKED_CAST")
    override fun list(index: Int): List<Any?> = translated { fromJava(java.listArg(index)) as List<Any?> }

    @Suppress("UNCHECKED_CAST")
    override fun map(index: Int): Map<Any?, Any?> = translated { fromJava(java.mapArg(index)) as Map<Any?, Any?> }
    override fun charge(units: Long): Boolean = java.charge(units)
}

internal class JvmSandbox(val java: JSandbox, private var current: JvmProgram) : Sandbox {

    override val program: Program
        get() {
            val p = java.program()
            return current.takeIf { it.java === p } ?: JvmProgram(p).also { current = it }
        }
    override val isClosed: Boolean get() = java.isClosed

    override fun bind(importName: String, function: HostFunction) = translated {
        java.bind(importName) { call -> toJava(function.call(JvmHostCall(call))) }
    }

    override fun start() = translated { java.start() }

    override fun call(function: Export, vararg args: Any?): Value = translated {
        JvmValue(java.call(export(function), *args.map(::toJava).toTypedArray()))
    }

    override fun spawn(function: Export, vararg args: Any?): TaskOutcome = translated {
        outcome(java.spawn(export(function), *args.map(::toJava).toTypedArray()))
    }

    override fun resume(task: Int, value: Any?): TaskOutcome = translated { outcome(java.resume(task, toJava(value))) }

    override fun cancel(task: Int) = translated { java.cancel(task) }
    override fun exportState(): String = translated { java.exportState() }
    override fun importState(json: String) = translated { java.importState(json) }
    override fun strLossy(text: ByteArray): Value = translated { JvmValue(java.strLossy(text)) }
    override fun reload(next: Program) = translated { java.reload(jvm(next).java) }
    override fun exportHandle(handle: Long): String = translated { java.exportHandle(handle) }
    override fun importHandle(handle: Long, json: String) = translated { java.importHandle(handle, json) }
    override fun dropHandle(handle: Long) = translated { java.dropHandle(handle) }

    override fun link(pkg: String, provider: Sandbox) = translated {
        java.link(pkg, (provider as? JvmSandbox ?: throw IllegalArgumentException("a sandbox of another runtime")).java)
    }

    override var fuelPerCall: Long
        get() = java.fuelPerCall()
        set(value) = java.setFuelPerCall(value)
    override val fuelLeft: Long get() = java.fuelLeft()
    override val memoryUsed: Long get() = java.memoryUsed()
    override val liveTasks: Long get() = java.liveTasks()

    override fun close() = java.close()

    private fun export(function: Export): JExport =
        (function as? JvmExport)?.java ?: throw IllegalArgumentException("$function belongs to another runtime")

    private fun outcome(o: JTaskOutcome): TaskOutcome =
        TaskOutcome(TaskOutcome.Kind.entries[o.kind().ordinal], o.task(), JvmValue(o.value()))
}
