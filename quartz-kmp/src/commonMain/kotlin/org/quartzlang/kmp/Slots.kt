package org.quartzlang.kmp

/** Converts between Kotlin values and one qz_slot by the kind the program declares. */
internal object Slots {
    /** Makes str and list slots in whatever arena is current, and checks values passed back. */
    interface Makers {
        fun string(utf8: ByteArray): Long

        fun list(type: Type, elements: List<*>): Long

        fun passBack(type: Type, value: Value): Long
    }

    /** Reads what str and list slots point to. */
    interface Readers {
        fun string(bits: Long): ByteArray

        fun list(type: Type, bits: Long): List<Any?>
    }

    /** A slot the program reads as [type]; throws IllegalArgumentException when [value] does not fit. */
    fun encode(type: Type, value: Any?, makers: Makers): Long {
        val kind = type.kind
        require(type.slots == 1) { "a $type value cannot cross the C interface yet" }
        if (value == null) {
            require(type.isNullableReference) { "null cannot be passed as $type" }
            return 0
        }
        if (value is Value) {
            return makers.passBack(type, value)
        }
        if (kind.isInteger) {
            return integer(kind, value)
        }
        return when (kind) {
            Kind.BOOL -> if (value is Boolean) (if (value) 1L else 0L) else throw mismatch(type, value)
            Kind.F64 -> floating(type, value).toRawBits()
            Kind.F32 -> floating(type, value).toFloat().toRawBits().toLong() and 0xFFFF_FFFFL
            Kind.CHAR -> codePoint(type, value)
            Kind.STR -> if (value is String) makers.string(value.encodeToByteArray()) else throw mismatch(type, value)
            Kind.BYTES -> if (value is ByteArray) makers.string(value) else throw mismatch(type, value)
            Kind.LIST -> if (value is List<*>) makers.list(type, value) else throw mismatch(type, value)
            Kind.NULLABLE -> {
                require(type.isNullableReference) { "a $type value cannot cross the C interface yet" }
                encode(type.element!!, value, makers)
            }
            Kind.HANDLE -> when (value) {
                is Long -> value
                is Int -> value.toLong()
                else -> throw mismatch(type, value)
            }
            else -> throw IllegalArgumentException("a $type value cannot cross the C interface yet")
        }
    }

    /** The slot boxed as its type says: Long, Float, Double, Boolean, Int (char), String, ByteArray, List or null. */
    fun box(type: Type, bits: Long, readers: Readers): Any? {
        val kind = type.kind
        if (kind.isInteger || kind == Kind.HANDLE) {
            return widen(kind, bits)
        }
        return when (kind) {
            Kind.VOID -> null
            Kind.BOOL -> bits != 0L
            Kind.F64 -> Double.fromBits(bits)
            Kind.F32 -> Float.fromBits(bits.toInt())
            Kind.CHAR -> bits.toInt()
            Kind.STR -> readers.string(bits).decodeToString()
            Kind.BYTES -> readers.string(bits)
            Kind.LIST -> readers.list(type, bits)
            Kind.NULLABLE -> {
                if (!type.isNullableReference) {
                    throw UnsupportedOperationException("a $type value cannot be read through the C interface yet")
                }
                if (bits == 0L) null else box(type.element!!, bits, readers)
            }
            else -> throw UnsupportedOperationException("a $kind value cannot be read through the C interface yet")
        }
    }

    /** An integer slot as a Long: signed kinds sign-extended from their width, unsigned ones zero-extended. */
    fun widen(kind: Kind, bits: Long): Long {
        val width = kind.bits
        if (width == 0 || width == 64) {
            return bits
        }
        val shift = 64 - width
        return if (kind.isSigned) bits shl shift shr shift else bits shl shift ushr shift
    }

    /** Whether a [value] of [from] may be read as [to] (the scalar accessors of Value and HostCall). */
    fun readable(from: Kind, to: Kind): Boolean = when (to) {
        Kind.I64 -> from.isInteger || from == Kind.HANDLE || from == Kind.CHAR || from == Kind.BOOL ||
            from == Kind.ENUM || from == Kind.OTHER
        Kind.F64 -> from == Kind.F64 || from == Kind.F32 || from == Kind.OTHER
        else -> from == to || from == Kind.OTHER
    }

    fun double(kind: Kind, bits: Long): Double =
        if (kind == Kind.F32) Float.fromBits(bits.toInt()).toDouble() else Double.fromBits(bits)

    private fun mismatch(type: Type, value: Any) =
        IllegalArgumentException("a ${value::class.simpleName} cannot be passed as $type")

    private fun integer(kind: Kind, value: Any): Long {
        val n = when (value) {
            is Long -> value
            is Int -> value.toLong()
            is Short -> value.toLong()
            is Byte -> value.toLong()
            is Char -> value.code.toLong()
            else -> throw IllegalArgumentException("a ${value::class.simpleName} cannot be passed as $kind")
        }
        return checkedInteger(kind, n)
    }

    fun checkedInteger(kind: Kind, n: Long): Long {
        val width = kind.bits
        val fits = width == 64 || if (kind.isSigned) {
            n >= -(1L shl (width - 1)) && n < 1L shl (width - 1)
        } else {
            n >= 0 && n < 1L shl width
        }
        require(fits) { "$n does not fit in ${kind.name.lowercase()}" }
        return n
    }

    private fun floating(type: Type, value: Any): Double = when (value) {
        is Double -> value
        is Float -> value.toDouble()
        is Long -> value.toDouble()
        is Int -> value.toDouble()
        is Short -> value.toDouble()
        is Byte -> value.toDouble()
        else -> throw mismatch(type, value)
    }

    private fun codePoint(type: Type, value: Any): Long {
        val cp = when (value) {
            is Char -> value.code
            is Int -> value
            else -> throw mismatch(type, value)
        }
        require(cp in 0..0x10FFFF && cp !in 0xD800..0xDFFF) { "U+${cp.toString(16)} is not a Unicode scalar value" }
        return cp.toLong()
    }
}
