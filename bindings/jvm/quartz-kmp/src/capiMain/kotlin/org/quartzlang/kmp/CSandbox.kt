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
    private var argv = LongArray(8)

    init {
        val sb = with(limits) { qzSandboxNew(current.address, memoryBytes, fuelPerCall, maxDepth, tier.ordinal) }
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
        val binding = CBinding(this, function, declared.name, declared.signature)
        val token = qzBind(address, declared.name, binding)
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
        val n = slots(f, args)
        val type = f.signature.result
        group.generation++
        if (type.slots > 1) {
            val wide = LongArray(type.slots)
            check(qzCallWide(address, f.index, argv, n, wide, type.slots))
            CValue(wide[0], this, group.generation, type, wide)
        } else {
            check(qzCall(address, f.index, argv, n, result))
            CValue(result[0], this, group.generation, type)
        }
    }

    override fun spawn(function: Export, vararg args: Any?): TaskOutcome = begin {
        val f = own(function)
        val n = slots(f, args)
        group.generation++
        check(qzSpawn(address, f.index, argv, n, outcome))
        taskResults[outcome[1].toInt()] = f.signature.result
        outcome()
    }

    override fun resume(task: Int, value: Any?): TaskOutcome = begin {
        val waitType = qzWaitType(address, task)
        require(waitType != 0) { "task $task is not waiting for a value" }
        val awaited = current.type(waitType)
        if (awaited.slots == 1 && !awaited.isComposite) {
            val slot = if (value == null) 0L else Slots.encode(awaited, value, makers)
            group.generation++
            check(qzResume(address, task, slot, outcome))
        } else {
            val slots = LongArray(maxOf(1, awaited.slots))
            Slots.encode(awaited, value, slots, 0, makers)
            group.generation++
            check(qzResumeWide(address, task, slots, awaited.slots, outcome))
        }
        outcome()
    }

    override fun cancel(task: Int) = begin {
        group.generation++
        taskResults.remove(task)
        qzCancel(address, task)
    }

    override fun strLossy(text: ByteArray): Value = open {
        CValue(newLossyString(text), this, group.generation, Type(Kind.STR))
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

    /** Reads str, list and map slots, checking no later call freed them; a negative generation skips the check. */
    fun readers(generation: Long): Slots.Readers = object : Slots.Readers {
        override fun string(bits: Long): ByteArray {
            if (generation >= 0) {
                requireCurrent(generation)
            }
            return qzStr(bits)
        }

        override fun list(type: Type, bits: Long): List<Any?> {
            if (generation >= 0) {
                requireCurrent(generation)
            }
            val e = type.element!!
            val cell = LongArray(maxOf(1, e.slots))
            return List(qzListLength(bits).toInt()) { i ->
                if (!qzListGet(bits, i.toLong(), cell, e.slots)) {
                    throw QuartzException("cannot read element $i of a $type")
                }
                Slots.decode(e, cell, 0, this)
            }
        }

        override fun map(type: Type, bits: Long): Map<Any?, Any?> {
            if (generation >= 0) {
                requireCurrent(generation)
            }
            val k = type.key!!
            val v = type.element!!
            val key = LongArray(maxOf(1, k.slots))
            val value = LongArray(maxOf(1, v.slots))
            val out = LinkedHashMap<Any?, Any?>()
            for (i in 0 until qzMapLength(bits)) {
                if (!qzMapEntry(address, bits, i, key, value)) {
                    throw QuartzException("cannot read entry $i of a $type")
                }
                out[Slots.decode(k, key, 0, this)] = Slots.decode(v, value, 0, this)
            }
            return out
        }
    }

    /** A list of [type] filled with [elements], as an argument or, in a host function, its result. */
    fun newList(type: Type, elements: List<*>, makers: Slots.Makers): Long {
        val e = type.element!!
        val list = qzSandboxList(address, type.id)
        if (list == 0L) {
            throw TrapException(TrapCode.OUT_OF_MEMORY, "cannot make a $type")
        }
        val cell = LongArray(maxOf(1, e.slots))
        for (x in elements) {
            Slots.encode(e, x, cell, 0, makers)
            if (!qzListPush(address, list, cell, e.slots)) {
                throw TrapException(TrapCode.OUT_OF_MEMORY, "cannot grow the list")
            }
        }
        return list
    }

    /** A map of [type] with [entries] in their iteration order. */
    fun newMap(type: Type, entries: Map<*, *>, makers: Slots.Makers): Long {
        val k = type.key!!
        val v = type.element!!
        val map = qzSandboxMap(address, type.id)
        if (map == 0L) {
            throw TrapException(TrapCode.OUT_OF_MEMORY, "cannot make a $type")
        }
        val key = LongArray(maxOf(1, k.slots))
        val value = LongArray(maxOf(1, v.slots))
        for ((ek, ev) in entries) {
            Slots.encode(k, ek, key, 0, makers)
            Slots.encode(v, ev, value, 0, makers)
            if (!qzMapSet(address, map, key, value)) {
                throw TrapException(TrapCode.OUT_OF_MEMORY, "cannot grow the map")
            }
        }
        return map
    }

    /** The slots of [value], a result of this sandbox, passed back as [type]. */
    fun passBack(type: Type, value: Value, out: LongArray, at: Int) {
        val kind = value.kind
        require(value.type == type || (kind.isInteger && type.kind.isInteger) ||
            (kind == Kind.STR && type.kind == Kind.BYTES)) { "a ${value.type} cannot be passed as $type" }
        require(kind != Kind.FUNCTION) { "a function cannot be passed back" }
        if (Slots.holdsReferences(value.type)) {
            require(value is CValue && value.ownedBy(this)) {
                "a result holding strings, lists or maps is only valid in its sandbox until the next call"
            }
        }
        if (kind.isInteger && type.kind.isInteger) {
            out[at] = Slots.checkedInteger(type.kind, Slots.widen(kind, value.bits))
            return
        }
        value.slots().copyInto(out, at, 0, minOf(type.slots, value.slots().size))
    }

    fun runHost(binding: CBinding, call: Long, args: Long, res: Long): Boolean {
        val hc = if (hostCall.active) CHostCall(this) else hostCall
        hc.begin(binding.signature, call, args, res)
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

    /** Fills [argv] with the arguments' slots and returns how many there are. */
    private fun slots(f: CExport, args: Array<out Any?>): Int {
        val signature = f.signature
        require(args.size == signature.arity) {
            "expected ${signature.arity} arguments ${signature.params}, got ${args.size}"
        }
        if (f.argSlots > argv.size) {
            argv = LongArray(maxOf(f.argSlots, argv.size * 2))
        }
        for (i in args.indices) {
            if (f.simple) {
                argv[i] = Slots.encode(signature.params[i], args[i], makers)
            } else {
                Slots.encode(signature.params[i], args[i], argv, f.offsets[i], makers)
            }
        }
        return f.argSlots
    }

    private val makers = object : Slots.Makers {
        override fun string(utf8: ByteArray): Long = newString(utf8)

        override fun bytes(data: ByteArray): Long = qzSandboxBytes(address, data).also {
            if (it == 0L) {
                throw TrapException(TrapCode.OUT_OF_MEMORY, "out of memory passing Bytes")
            }
        }

        override fun stringLossy(text: ByteArray): Long = newLossyString(text)

        override fun list(type: Type, elements: List<*>): Long = newList(type, elements, this)

        override fun map(type: Type, entries: Map<*, *>): Long = newMap(type, entries, this)

        override fun passBack(type: Type, value: Value, out: LongArray, at: Int) =
            this@CSandbox.passBack(type, value, out, at)
    }

    private fun newLossyString(text: ByteArray): Long {
        val slot = qzSandboxStrLossy(address, text)
        if (slot == 0L) {
            throw TrapException(TrapCode.OUT_OF_MEMORY, "out of memory passing a string")
        }
        return slot
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
        val type =
            if (kind == TaskOutcome.Kind.DONE) taskResults.remove(task) ?: Type(Kind.OTHER) else Type(Kind.HANDLE)
        if (kind == TaskOutcome.Kind.DONE && type.slots > 1) {
            val wide = LongArray(type.slots)
            check(qzTaskResult(address, task, wide, type.slots))
            return TaskOutcome(kind, task, CValue(wide[0], this, group.generation, type, wide))
        }
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
