package org.quartzlang.kmp

/** Converts between Kotlin values and the slots of a Quartz value by the type the program declares. */
internal object Slots {
    /** Makes str, list and map slots in whatever arena is current, and checks values passed back. */
    interface Makers {
        fun string(utf8: ByteArray): Long

        fun bytes(data: ByteArray): Long

        fun stringLossy(text: ByteArray): Long

        /** A list of [type] holding [count] elements laid out one after another in [slots]. */
        fun list(type: Type, slots: LongArray, count: Int): Long

        /** A map of [type] with [count] entries, keys and values laid out like [list]. */
        fun map(type: Type, keys: LongArray, values: LongArray, count: Int): Long

        /** The slots of a result of this sandbox passed back as [type], written at [out] from [at]. */
        fun passBack(type: Type, value: Value, out: LongArray, at: Int)
    }

    /** Reads what str, list and map slots point to. */
    interface Readers {
        fun string(bits: Long): ByteArray

        fun listLength(bits: Long): Int

        /** Copies element [index] of a list of [type] into [out]. */
        fun listGet(type: Type, bits: Long, index: Int, out: LongArray)

        fun mapLength(bits: Long): Long

        /** Copies entry [index] of a map of [type] into [key] and [value]. */
        fun mapEntry(type: Type, bits: Long, index: Long, key: LongArray, value: LongArray)
    }

    /** One slot of a one-slot type; throws IllegalArgumentException when [value] does not fit. */
    fun encode(type: Type, value: Any?, makers: Makers): Long {
        if (type.kind == Kind.I64 && value is Long) {
            return value
        }
        val container = type.kind == Kind.LIST || type.kind == Kind.MAP
        if (!type.isComposite && value !is Value && !container) {
            return scalar(type, value, makers)
        }
        val out = LongArray(maxOf(1, type.slots))
        encode(type, value, out, 0, makers)
        return out[0]
    }

    /** Writes [value] as [type] into `out[at until at + type.slots]`. */
    fun encode(type: Type, value: Any?, out: LongArray, at: Int, makers: Makers) {
        SlotWriter.encode(type, value, out, at, makers)
    }

    /** Reads a value of [type] from `slots[at..]`; see [Value.get] for what each type gives. */
    fun decode(type: Type, slots: LongArray, at: Int, readers: Readers): Any? =
        SlotReader.decode(type, slots, at, readers)

    /** A one-slot value as the Kotlin value it maps to. */
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
            Kind.LIST, Kind.MAP, Kind.STRUCT, Kind.TUPLE, Kind.ENUM, Kind.NULLABLE ->
                decode(type, longArrayOf(bits), 0, readers)
            else -> throw UnsupportedOperationException("a $kind value cannot be read through the C interface")
        }
    }

    /** True when the type, or anything inside it, points into the sandbox's memory. */
    fun holdsReferences(type: Type): Boolean = type.holdsReferences

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

    fun scalar(type: Type, value: Any?, makers: Makers): Long {
        val kind = type.kind
        requireNotNull(value) { "null cannot be passed as $type" }
        if (kind.isInteger) {
            return integer(kind, value)
        }
        return when (kind) {
            Kind.BOOL -> if (value is Boolean) (if (value) 1L else 0L) else throw mismatch(type, value)
            Kind.F64 -> floating(type, value).toRawBits()
            Kind.F32 -> floating(type, value).toFloat().toRawBits().toLong() and 0xFFFF_FFFFL
            Kind.CHAR -> codePoint(type, value)
            Kind.STR -> when (value) {
                is String -> makers.string(value.encodeToByteArray())
                is LossyText -> makers.stringLossy(value.raw)
                else -> throw mismatch(type, value)
            }
            Kind.BYTES -> if (value is ByteArray) makers.bytes(value) else throw mismatch(type, value)
            Kind.LIST, Kind.MAP -> throw mismatch(type, value)
            Kind.HANDLE -> when (value) {
                is Long -> value
                is Int -> value.toLong()
                else -> throw mismatch(type, value)
            }
            else -> throw IllegalArgumentException("a $type value cannot cross the C interface")
        }
    }

    fun structValues(type: Type, value: Any?): List<Any?> {
        val names = type.fields.map { it.name }
        return when (value) {
            is Struct -> {
                require(sameType(type.name, value.typeName)) {
                    "a struct ${value.typeName} cannot be passed as ${type.name}"
                }
                names.map { name ->
                    val i = value.names.indexOf(name).takeIf { it >= 0 }
                        ?: value.names.indexOfFirst { normalize(it) == normalize(name) }
                    require(i >= 0) { "the struct passed lacks field $name" }
                    value.values[i]
                }
            }
            is Map<*, *> -> {
                for (k in value.keys) {
                    require(k is String && k in names) { "the struct has no field $k; it has $names" }
                }
                type.fields.map { f ->
                    require(f.name in value || f.type.kind == Kind.NULLABLE) {
                        "the map passed as a struct lacks field ${f.name}"
                    }
                    value[f.name]
                }
            }
            null -> throw IllegalArgumentException("null cannot be passed as $type")
            else -> throw notGeneric(type, value)
        }
    }

    fun tupleValues(type: Type, value: Any?): List<Any?> {
        val values = when (value) {
            is List<*> -> value
            is Array<*> -> value.asList()
            is Pair<*, *> -> value.toList()
            is Triple<*, *, *> -> value.toList()
            is Struct -> value.values
            null -> throw IllegalArgumentException("null cannot be passed as $type")
            else -> throw notGeneric(type, value)
        }
        require(values.size == type.fields.size) {
            "a tuple of ${type.fields.size} cannot take ${values.size} values"
        }
        return values
    }

    fun variantOf(type: Type, value: Any?): Variant = when {
        value is Variant -> value
        value is Result.Ok<*> -> Variant("Ok", listOf(value.value))
        value is Result.Err<*> -> Variant("Err", listOf(value.error))
        type.isResult -> Variant("Ok", listOf(value))
        value is String -> Variant(value)
        value is Enum<*> -> Variant(value.name)
        value == null -> throw IllegalArgumentException("null cannot be passed as $type")
        else -> throw notGeneric(type, value)
    }

    private fun notGeneric(type: Type, value: Any) = IllegalArgumentException(
        "a ${value::class.simpleName} cannot be passed as $type: pass a Struct, Map, List, Variant or Result, " +
            "or a @Serializable value through quartzValue()",
    )

    fun mismatch(type: Type, value: Any) =
        IllegalArgumentException("a ${value::class.simpleName} cannot be passed as $type")

    fun integer(kind: Kind, value: Any): Long {
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

    fun floating(type: Type, value: Any): Double = when (value) {
        is Double -> value
        is Float -> value.toDouble()
        is Long -> value.toDouble()
        is Int -> value.toDouble()
        is Short -> value.toDouble()
        is Byte -> value.toDouble()
        else -> throw mismatch(type, value)
    }

    fun codePoint(type: Type, value: Any): Long {
        val cp = when (value) {
            is Char -> value.code
            is Int -> value
            else -> throw mismatch(type, value)
        }
        require(cp in 0..0x10FFFF && cp !in 0xD800..0xDFFF) { "U+${cp.toString(16)} is not a Unicode scalar value" }
        return cp.toLong()
    }
}
