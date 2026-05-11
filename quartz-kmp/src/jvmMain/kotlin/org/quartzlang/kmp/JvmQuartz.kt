package org.quartzlang.kmp

import java.util.concurrent.ConcurrentHashMap
import org.quartzlang.jvm.CompileException as JCompileException
import org.quartzlang.jvm.Export as JExport
import org.quartzlang.jvm.Kind as JKind
import org.quartzlang.jvm.Limits as JLimits
import org.quartzlang.jvm.Program as JProgram
import org.quartzlang.jvm.Quartz as JQuartz
import org.quartzlang.jvm.QuartzException as JQuartzException
import org.quartzlang.jvm.Sandbox as JSandbox
import org.quartzlang.jvm.Signature as JSignature
import org.quartzlang.jvm.TaskOutcome as JTaskOutcome
import org.quartzlang.jvm.TrapException as JTrapException
import org.quartzlang.jvm.Value as JValue

internal actual fun platformAbiVersion(): Int = JQuartz.abiVersion()

internal actual fun platformCompile(sources: List<Source>, options: CompileOptions): Program = translated {
    val opts = org.quartzlang.jvm.CompileOptions(options.emitBinary, options.keepLines, options.manifest)
    JvmProgram(JQuartz.compile(sources.map { org.quartzlang.jvm.Source(it.name, it.text) }, opts))
}

internal actual fun platformLoad(bytes: ByteArray, trustedKeys: List<ByteArray>, requireSignature: Boolean): Program =
    translated { JvmProgram(JProgram.load(bytes, trustedKeys, requireSignature)) }

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
    throw CompileException(e.diagnostics())
} catch (e: JTrapException) {
    throw TrapException(TrapCode.entries[e.code().ordinal], e.message, e.cause)
} catch (e: JQuartzException) {
    throw QuartzException(e.message, e.cause)
}

internal fun kindOf(kind: JKind): Kind = Kind.entries[kind.ordinal]

internal fun jkind(kind: Kind): JKind = JKind.entries[kind.ordinal]

internal fun signatureOf(s: JSignature): Signature =
    Signature(s.params().map { Type(kindOf(it.kind()), it.slots()) }, Type(kindOf(s.result().kind()), s.result().slots()))

internal class JvmProgram(val java: JProgram) : Program {
    private val exports = ConcurrentHashMap<String, JvmExport>()

    override val diagnostics: String get() = java.diagnostics()
    override val imports: List<Import> by lazy {
        translated { java.imports().map { Import(it.name(), signatureOf(it.signature())) } }
    }

    override fun binary(): ByteArray? = translated { java.binary().orElse(null) }

    override fun find(name: String): Export? = exports[name] ?: translated {
        val found: JExport? = java.find(name).orElse(null)
        found?.let { JvmExport(this, it) }?.also { exports[name] = it }
    }

    override fun packageInterface(manifest: String?): String = translated { java.packageInterface(manifest) }

    override fun close(): Unit = java.close()
}

internal class JvmExport(val program: JvmProgram, val java: JExport) : Export {
    override val name: String get() = java.name()
    override val signature: Signature = signatureOf(java.signature())
}

internal class JvmValue(val java: JValue) : Value {
    override val type: Type = Type(kindOf(java.kind()), java.type().slots())
    override val bits: Long get() = java.bits()
    override val isCurrent: Boolean get() = java.isCurrent
    override fun asLong(): Long = java.asLong()
    override fun asDouble(): Double = java.asDouble()
    override fun asFloat(): Float = java.asFloat()
    override fun asBoolean(): Boolean = java.asBoolean()
    override fun asCodePoint(): Int = java.asCodePoint()
    override fun asString(): String = java.asString()
    override fun asBytes(): ByteArray = java.asBytes()
    override fun get(): Any? = java.get()
    override fun listLength(): Int = java.listLength()
    override fun asList(element: Kind): List<Any?> = java.asList(jkind(element))
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
    override fun arg(index: Int): Any? = java.arg(index)
    override fun list(index: Int, element: Kind): List<Any?> = java.listArg(index, jkind(element))
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
        java.bind(importName) { call -> unwrap(function.call(JvmHostCall(call))) }
    }

    override fun start() = translated { java.start() }

    override fun call(function: Export, vararg args: Any?): Value = translated {
        JvmValue(java.call(export(function), *args.map(::unwrap).toTypedArray()))
    }

    override fun spawn(function: Export, vararg args: Any?): TaskOutcome = translated {
        outcome(java.spawn(export(function), *args.map(::unwrap).toTypedArray()))
    }

    override fun resume(task: Int, value: Any?): TaskOutcome = translated { outcome(java.resume(task, unwrap(value))) }

    override fun cancel(task: Int) = translated { java.cancel(task) }
    override fun exportState(): String = translated { java.exportState() }
    override fun importState(json: String) = translated { java.importState(json) }
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

    private fun unwrap(value: Any?): Any? = when (value) {
        is JvmValue -> value.java
        is Unit -> null
        else -> value
    }
}
