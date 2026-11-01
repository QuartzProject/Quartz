package org.quartzlang.kmp

internal class CValue(
    override val bits: Long,
    private val owner: CSandbox,
    private val generation: Long,
    override val type: Type,
    private val wide: LongArray? = null,
) : Value {
    override val isCurrent: Boolean get() = owner.isCurrent(generation)

    internal fun ownedBy(sandbox: CSandbox): Boolean = owner === sandbox && owner.isCurrent(generation)

    override fun asLong(): Long {
        require(Kind.I64, "asLong")
        return Slots.widen(kind, bits)
    }

    override fun asDouble(): Double {
        require(Kind.F64, "asDouble")
        return Slots.double(kind, bits)
    }

    override fun asFloat(): Float = asDouble().toFloat()

    override fun asBoolean(): Boolean {
        require(Kind.BOOL, "asBoolean")
        return bits != 0L
    }

    override fun asCodePoint(): Int {
        require(Kind.CHAR, "asCodePoint")
        return bits.toInt()
    }

    override fun asString(): String = asBytes().decodeToString()

    override fun asBytes(): ByteArray {
        check(kind == Kind.STR || kind == Kind.BYTES) { "the value is $type, asBytes does not apply" }
        return owner.readString(bits, generation)
    }

    override fun get(): Any? {
        if (wide == null && type.slots > 1) {
            throw UnsupportedOperationException("only the first slot of this $type is known")
        }
        val readers = owner.readers(generation)
        return if (wide != null) Slots.decode(type, wide, 0, readers) else Slots.box(type, bits, readers)
    }

    override fun listLength(): Int {
        check(kind == Kind.LIST) { "the value is $type, not a list" }
        return owner.listLength(bits, generation)
    }

    override fun asList(): List<Any?> {
        check(kind == Kind.LIST) { "the value is $type, not a list" }
        return owner.readers(generation).list(type, bits)
    }

    override fun slots(): LongArray = wide?.copyOf() ?: longArrayOf(bits)

    private fun require(to: Kind, what: String) {
        check(Slots.readable(kind, to)) { "the value is $type, $what does not apply" }
    }

    override fun toString(): String = "Value[$type 0x${bits.toULong().toString(16)}]"
}

internal class CBinding(val sandbox: CSandbox, val function: HostFunction, val name: String, var signature: Signature) {
    /** Called from native code; never throws, since nothing can catch it there. */
    fun dispatch(call: Long, args: Long, result: Long): Boolean = try {
        sandbox.runHost(this, call, args, result)
    } catch (t: Throwable) {
        qzCallFail(call, "host error")
        false
    }
}

internal class CHostCall(private val sandbox: CSandbox) : HostCall {
    private var sig: Signature? = null
    private var call = 0L
    private var args = 0L
    private var result = 0L
    val active: Boolean get() = sig != null

    override val signature: Signature get() = sig ?: throw IllegalStateException("the host call is over")

    fun begin(s: Signature, nativeCall: Long, nativeArgs: Long, nativeResult: Long) {
        sig = s
        call = nativeCall
        args = nativeArgs
        result = nativeResult
    }

    fun end() {
        sig = null
        call = 0
        args = 0
    }

    override fun long(index: Int): Long = Slots.widen(expect(index, Kind.I64, "long"), raw(index))

    override fun double(index: Int): Double = Slots.double(expect(index, Kind.F64, "double"), raw(index))

    override fun bool(index: Int): Boolean {
        expect(index, Kind.BOOL, "bool")
        return raw(index) != 0L
    }

    override fun codePoint(index: Int): Int {
        expect(index, Kind.CHAR, "codePoint")
        return raw(index).toInt()
    }

    override fun string(index: Int): String = bytes(index).decodeToString()

    override fun bytes(index: Int): ByteArray {
        val k = kindAt(index)
        check(k == Kind.STR || k == Kind.BYTES) { "argument $index is $k, bytes does not apply" }
        return qzStr(raw(index))
    }

    override fun arg(index: Int): Any? {
        kindAt(index)
        val type = signature.params[index]
        if (type.slots == 1 && !type.isComposite) {
            return Slots.box(type, raw(index), sandbox.readers(-1))
        }
        val at = signature.offset(index)
        val slots = LongArray(maxOf(1, type.slots)) { if (it < type.slots) qzSlot(args, at + it) else 0L }
        return Slots.decode(type, slots, 0, sandbox.readers(-1))
    }

    override fun map(index: Int): Map<Any?, Any?> {
        check(kindAt(index) == Kind.MAP) { "argument $index is ${kindAt(index)}, not a map" }
        return sandbox.readers(-1).map(signature.params[index], raw(index))
    }

    override fun list(index: Int): List<Any?> {
        check(kindAt(index) == Kind.LIST) { "argument $index is ${kindAt(index)}, not a list" }
        return sandbox.readers(-1).list(signature.params[index], raw(index))
    }

    override fun charge(units: Long): Boolean {
        signature
        return qzCallCharge(call, units)
    }

    fun setResult(value: Any?) {
        val type = signature.result
        if (type.kind == Kind.VOID) {
            return
        }
        if (type.slots == 1 && !type.isComposite) {
            qzSetSlot(result, 0, Slots.encode(type, value, makers))
            return
        }
        val slots = LongArray(maxOf(1, type.slots))
        Slots.encode(type, value, slots, 0, makers)
        for (i in 0 until type.slots) {
            qzSetSlot(result, i, slots[i])
        }
    }

    private val makers = object : Slots.Makers {
        override fun bytes(data: ByteArray): Long = qzCallBytes(call, data).also {
            if (it == 0L) {
                throw TrapException(TrapCode.OUT_OF_MEMORY, "out of memory returning Bytes")
            }
        }

        override fun string(utf8: ByteArray): Long = qzCallStr(call, utf8).also {
            if (it == 0L) {
                throw TrapException(TrapCode.OUT_OF_MEMORY, "out of memory returning a string")
            }
        }

        override fun stringLossy(text: ByteArray): Long = qzCallStrLossy(call, text).also {
            if (it == 0L) {
                throw TrapException(TrapCode.OUT_OF_MEMORY, "out of memory returning a string")
            }
        }

        override fun list(type: Type, elements: List<*>): Long = sandbox.newList(type, elements, this)

        override fun map(type: Type, entries: Map<*, *>): Long = sandbox.newMap(type, entries, this)

        override fun passBack(type: Type, value: Value, out: LongArray, at: Int) =
            throw IllegalArgumentException("a Value cannot be returned from a host function")
    }

    private fun kindAt(index: Int): Kind {
        val s = signature
        if (index !in 0 until s.arity) {
            throw IndexOutOfBoundsException("argument $index of ${s.arity}")
        }
        return s.params[index].kind
    }

    private fun expect(index: Int, to: Kind, what: String): Kind {
        val k = kindAt(index)
        check(Slots.readable(k, to) && k != Kind.OTHER) { "argument $index is $k, $what does not apply" }
        return k
    }

    private fun raw(index: Int): Long = qzSlot(args, signature.offset(index))
}
