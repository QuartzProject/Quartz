package org.quartzlang.kmp

/** Linked sandboxes share one generation: a call into one may reset the arenas of the others. */
internal class CGroup {
    var generation = 0L
    val members = mutableListOf<CSandbox>()
}

/** A sandbox over the C interface; not thread-confined like the JVM one, and only [close] frees it. */
internal class CSandbox(private var current: CProgram, limits: Limits) : Sandbox {
    private val handle: CHandle
    private var group = CGroup().also { it.members += this }
    private var active = false
    private val bindings = mutableListOf<CBinding>()
    private val taskResults = mutableMapOf<Int, Type>()
    private val hostCall = CHostCall(this)
    private val result = LongArray(1)
    private val outcome = LongArray(3)
    private val element = LongArray(1)
    private var argv = LongArray(8)

    init {
        val sb = qzSandboxNew(current.address, limits.memoryBytes, limits.fuelPerCall, limits.maxDepth, limits.tier.ordinal)
        if (sb == 0L) {
            throw QuartzException("cannot create a sandbox")
        }
        handle = CHandle(sb, ::qzSandboxFree)
    }

    override val program: Program get() = current
    override var isClosed: Boolean = false
        private set

    private val address: Long get() = handle.address

    override fun bind(importName: String, function: HostFunction) = begin {
        val declared = current.findImport(importName)
            ?: throw IllegalArgumentException("the program imports nothing named $importName")
        val binding = CBinding(this, function, importName, declared.signature)
        val token = qzBind(address, importName, binding)
        if (token == 0L) {
            throw IllegalArgumentException("the program imports nothing named $importName")
        }
        bindings += binding
        handle.afterFree { qzUnbind(token) }
    }

    override fun start() = begin {
        group.generation++
        check(qzStart(address))
    }

    override fun call(function: Export, vararg args: Any?): Value = begin {
        val f = own(function)
        val n = slots(f.signature, args)
        group.generation++
        check(qzCall(address, f.index, argv, n, result))
        CValue(result[0], this, group.generation, f.signature.result)
    }

    override fun spawn(function: Export, vararg args: Any?): TaskOutcome = begin {
        val f = own(function)
        val n = slots(f.signature, args)
        group.generation++
        check(qzSpawn(address, f.index, argv, n, outcome))
        taskResults[outcome[1].toInt()] = f.signature.result
        outcome()
    }

    override fun resume(task: Int, value: Any?): TaskOutcome = begin {
        val slot = when (value) {
            null -> 0L
            is List<*> -> newList(task, value)
            else -> loose(value)
        }
        group.generation++
        check(qzResume(address, task, slot, outcome))
        outcome()
    }

    override fun cancel(task: Int) = begin {
        group.generation++
        taskResults.remove(task)
        qzCancel(address, task)
    }

    override fun exportState(): String = begin {
        qzExportState(address) ?: throw TrapException(TrapCode.of(qzTrapCode(address)), error())
    }

    override fun importState(json: String) = begin {
        group.generation++
        check(qzImportState(address, json))
    }

    override fun reload(next: Program) = begin {
        val p = next as? CProgram ?: throw IllegalArgumentException("a program of another runtime")
        val before = bindings.associateWith { it.signature }
        bindings.forEach { b -> p.findImport(b.name)?.let { b.signature = it.signature } }
        group.generation++
        try {
            check(qzReload(address, p.address))
        } catch (t: Throwable) {
            before.forEach { (b, s) -> b.signature = s }
            throw t
        }
        current = p
        taskResults.clear()
    }

    override fun exportHandle(handle: Long): String = begin {
        qzExportHandle(address, handle) ?: throw TrapException(TrapCode.of(qzTrapCode(address)), error())
    }

    override fun importHandle(handle: Long, json: String) = begin {
        group.generation++
        check(qzImportHandle(address, handle, json))
    }

    override fun dropHandle(handle: Long) = begin { qzDropHandle(address, handle) }

    override fun link(pkg: String, provider: Sandbox) = begin {
        val other = provider as? CSandbox ?: throw IllegalArgumentException("a sandbox of another runtime")
        require(other !== this) { "a sandbox cannot link to itself" }
        check(!other.isClosed && !other.active) { "the provider is closed or running a call" }
        if (qzLink(address, pkg, other.address) != QZ_OK) {
            throw QuartzException(error())
        }
        handle.dependOn(other.handle)
        if (other.group !== group) {
            val absorbed = other.group
            group.generation = maxOf(group.generation, absorbed.generation) + 1
            absorbed.members.forEach { it.group = group; group.members += it }
        }
    }

    override var fuelPerCall: Long
        get() = open { qzFuelPerCall(address) }
        set(value) = open { qzSetFuel(address, value) }
    override val fuelLeft: Long get() = open { qzFuelLeft(address) }
    override val memoryUsed: Long get() = open { qzMemoryUsed(address) }
    override val liveTasks: Long get() = open { qzLiveTasks(address) }

    override fun close() {
        if (isClosed) {
            return
        }
        check(!active) { "a sandbox cannot be closed from inside its own call" }
        isClosed = true
        group.members.remove(this)
        handle.release()
    }

    fun isCurrent(generation: Long): Boolean = !isClosed && group.generation == generation

    fun readString(bits: Long, generation: Long): ByteArray {
        requireCurrent(generation)
        return qzStr(bits)
    }

    fun listLength(bits: Long, generation: Long): Int {
        requireCurrent(generation)
        return qzListLength(bits).toInt()
    }

    fun readList(bits: Long, generation: Long, element: Kind): List<Any?> {
        requireCurrent(generation)
        return readListSlot(bits, element)
    }

    fun readListSlot(bits: Long, kind: Kind): List<Any?> = List(qzListLength(bits).toInt()) { i ->
        if (!qzListGet(bits, i.toLong(), element)) {
            throw UnsupportedOperationException("list elements wider than one slot cannot be read yet")
        }
        Slots.box(kind, element[0], ::qzStr)
    }

    fun runHost(binding: CBinding, call: Long, args: Long): Boolean {
        val hc = if (hostCall.active) CHostCall(this) else hostCall
        hc.begin(binding.signature, call, args)
        return try {
            hc.setResult(binding.function.call(hc))
            true
        } catch (t: Throwable) {
            hostFailure = t
            qzCallFail(call, describe(t))
            false
        } finally {
            hc.end()
        }
    }

    private var hostFailure: Throwable? = null

    private fun describe(t: Throwable): String = when {
        t is QuartzException && t.message != null -> t.message!!
        t.message == null -> t::class.simpleName ?: "host error"
        else -> "${t::class.simpleName}: ${t.message}"
    }

    private fun own(function: Export): CExport {
        val f = function as? CExport ?: throw IllegalArgumentException("$function belongs to another runtime")
        require(f.program === current) { "$function belongs to another program" }
        return f
    }

    private fun slots(signature: Signature, args: Array<out Any?>): Int {
        require(args.size == signature.arity) {
            "expected ${signature.arity} arguments ${signature.params}, got ${args.size}"
        }
        if (args.size > argv.size) {
            argv = LongArray(maxOf(args.size, argv.size * 2))
        }
        for (i in args.indices) {
            argv[i] = Slots.encode(signature.params[i], args[i], ::passBack, ::newString)
        }
        return args.size
    }

    private fun passBack(v: Value): Long {
        val kind = v.kind
        if (kind.isInteger) {
            return v.bits
        }
        require(kind != Kind.LIST && kind != Kind.MAP && kind != Kind.FUNCTION) { "a $kind result cannot be passed" }
        if (kind == Kind.STR || kind == Kind.BYTES) {
            require(v is CValue && v.ownedBy(this)) { "a str result only goes back to its own sandbox before its next call" }
        }
        return v.bits
    }

    private fun loose(value: Any): Long = Slots.encode(Type(Slots.looseKind(value)), value, ::passBack, ::newString)

    private fun newList(task: Int, elements: List<*>): Long {
        val list = qzSandboxList(address, qzWaitType(address, task))
        require(list != 0L) { "task $task does not wait for a list" }
        for (e in elements) {
            if (!qzListPush(address, list, loose(requireNotNull(e) { "list elements cannot be null" }))) {
                throw TrapException(TrapCode.OUT_OF_MEMORY, "cannot grow the list (quota or element size)")
            }
        }
        return list
    }

    private fun newString(utf8: ByteArray): Long {
        val slot = qzSandboxStr(address, utf8)
        if (slot == 0L) {
            throw TrapException(TrapCode.OUT_OF_MEMORY, "out of memory passing a string")
        }
        return slot
    }

    private fun outcome(): TaskOutcome {
        val kind = TaskOutcome.Kind.entries.getOrElse(outcome[0].toInt()) { TaskOutcome.Kind.YIELD }
        val task = outcome[1].toInt()
        val type = if (kind == TaskOutcome.Kind.DONE) taskResults.remove(task) ?: Type(Kind.OTHER) else Type(Kind.HANDLE)
        return TaskOutcome(kind, task, CValue(outcome[2], this, group.generation, type))
    }

    private fun check(status: Int) {
        if (status == QZ_OK) {
            return
        }
        val cause = hostFailure
        hostFailure = null
        if (status == QZ_TRAP) {
            throw TrapException(TrapCode.of(qzTrapCode(address)), error(), cause)
        }
        throw QuartzException(error())
    }

    private fun error(): String = qzError(address).ifEmpty { "unknown error" }

    private fun requireCurrent(generation: Long) {
        check(!isClosed) { "sandbox is closed" }
        check(group.generation == generation) { "a later call freed this result; copy it out before calling again" }
    }

    private inline fun <R> open(block: () -> R): R {
        check(!isClosed) { "sandbox is closed" }
        return block()
    }

    private inline fun <R> begin(block: () -> R): R {
        check(!isClosed) { "sandbox is closed" }
        check(!group.members.any { it.active }) { "sandbox is already running a call; it cannot be re-entered" }
        active = true
        hostFailure = null
        try {
            return block()
        } finally {
            active = false
        }
    }
}
