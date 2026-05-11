package org.quartzlang.kmp

internal class CValue(
    override val bits: Long,
    private val owner: CSandbox,
    private val generation: Long,
    override val type: Type,
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

    override fun get(): Any? = when (kind) {
        Kind.STR -> asString()
        Kind.BYTES -> asBytes()
        else -> Slots.box(kind, bits) { error("unreachable") }
    }

    override fun listLength(): Int {
        check(kind == Kind.LIST) { "the value is $type, not a list" }
        return owner.listLength(bits, generation)
    }

    override fun asList(element: Kind): List<Any?> {
        check(kind == Kind.LIST) { "the value is $type, not a list" }
        return owner.readList(bits, generation, element)
    }

    private fun require(to: Kind, what: String) {
        check(Slots.readable(kind, to)) { "the value is $type, $what does not apply" }
    }

    override fun toString(): String = "Value[$type 0x${bits.toULong().toString(16)}]"
}

internal class CBinding(val sandbox: CSandbox, val function: HostFunction, val name: String, var signature: Signature) {
    /** Called from native code; never throws, since nothing can catch it there. */
    fun dispatch(call: Long, args: Long): Boolean = try {
        sandbox.runHost(this, call, args)
    } catch (t: Throwable) {
        qzCallFail(call, "host error")
        false
    }
}

internal class CHostCall(private val sandbox: CSandbox) : HostCall {
    private var sig: Signature? = null
    private var call = 0L
    private var args = 0L
    val active: Boolean get() = sig != null

    override val signature: Signature get() = sig ?: throw IllegalStateException("the host call is over")

    fun begin(s: Signature, nativeCall: Long, nativeArgs: Long) {
        sig = s
        call = nativeCall
        args = nativeArgs
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

    override fun arg(index: Int): Any? = when (kindAt(index)) {
        Kind.STR -> string(index)
        Kind.BYTES -> bytes(index)
        else -> Slots.box(kindAt(index), raw(index)) { error("unreachable") }
    }

    override fun list(index: Int, element: Kind): List<Any?> {
        check(kindAt(index) == Kind.LIST) { "argument $index is ${kindAt(index)}, not a list" }
        return sandbox.readListSlot(raw(index), element)
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
        val bits = Slots.encode(type, value, { throw IllegalArgumentException("a Value cannot be returned") }) {
            qzCallStr(call, it).also { s -> if (s == 0L) throw TrapException(TrapCode.OUT_OF_MEMORY, "out of memory") }
        }
        // A direct binding's one-slot result is overwritten by HostCall::result after it returns, so set that.
        qzCallSetResult(call, bits)
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
