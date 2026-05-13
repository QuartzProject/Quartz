package org.quartzlang.kmp

/** The kind of a Quartz type as the C interface reports it, in the order of `qz_kind`. */
public enum class Kind {
    VOID, BOOL, I8, I16, I32, I64, U8, U16, U32, U64, F32, F64, CHAR, STR, BYTES, LIST, MAP, STRUCT, ENUM, NULLABLE,
    TUPLE, HANDLE, FUNCTION, OTHER;

    public val isInteger: Boolean get() = ordinal in I8.ordinal..U64.ordinal
    public val isSigned: Boolean get() = ordinal in I8.ordinal..I64.ordinal

    /** Bits of an integer kind, 0 for any other. */
    public val bits: Int
        get() = when (this) {
            I8, U8 -> 8
            I16, U16 -> 16
            I32, U32 -> 32
            I64, U64 -> 64
            else -> 0
        }

    public companion object {
        public fun of(code: Int): Kind = entries.getOrElse(code) { OTHER }
    }
}

/** Why a program trapped, in the order of `qz_trap`. */
public enum class TrapCode {
    NONE, UNREACHABLE, DIVIDE_BY_ZERO, INDEX_OUT_OF_BOUNDS, NULL_UNWRAP, OUT_OF_FUEL, OUT_OF_MEMORY, STACK_OVERFLOW,
    INVALID_HANDLE, PANIC, HOST_ERROR;

    public companion object {
        public fun of(code: Int): TrapCode = entries.getOrElse(code) { PANIC }
    }
}

/** A type: its kind, the slots it takes (a struct takes one per field), its id and the element of a list or nullable. */
public data class Type(val kind: Kind, val slots: Int = 1, val id: Int = 0, val element: Type? = null) {
    /** A one-slot nullable str, Bytes or list, which is null or a pointer. */
    val isNullableReference: Boolean
        get() = kind == Kind.NULLABLE && slots == 1 && element?.kind in setOf(Kind.STR, Kind.BYTES, Kind.LIST, Kind.MAP)

    override fun equals(other: Any?): Boolean =
        other is Type && other.kind == kind && other.slots == slots && other.element == element

    override fun hashCode(): Int = kind.hashCode() * 31 + slots

    override fun toString(): String {
        val name = kind.name.lowercase() + (element?.let { "<$it>" } ?: "")
        return if (slots == 1) name else "$name[$slots]"
    }
}

/** What a caller passes (`state` parameters left out) and what comes back. */
public data class Signature(val params: List<Type>, val result: Type) {
    val arity: Int get() = params.size

    /** Slot offset of parameter [index] in the argument array. */
    public fun offset(index: Int): Int = params.take(index).sumOf { it.slots }

    override fun toString(): String = "$params -> $result"
}

/** A host function a program imports, by its binding name; the runtime binds [builtIn] ones (the standard library). */
public data class Import(val name: String, val signature: Signature, val builtIn: Boolean = false)

/** One source file; a name ending in `.qzi` is a host interface. */
public data class Source(val name: String, val text: String)

/** How to compile: [emitBinary] also produces the .qzb module, [manifest] is package.toml text. */
public data class CompileOptions(
    val emitBinary: Boolean = false,
    val keepLines: Boolean = true,
    val manifest: String? = null,
)

/** What a sandbox may spend; zero keeps Quartz's default for that field. */
public data class Limits(
    val memoryBytes: Long = 0,
    val fuelPerCall: Long = 0,
    val maxDepth: Int = 0,
    val tier: Tier = Tier.DEFAULT,
) {
    init {
        require(memoryBytes >= 0 && fuelPerCall >= 0 && maxDepth >= 0) { "limits cannot be negative" }
    }

    public enum class Tier { DEFAULT, INTERPRETER, AUTOMATIC, NATIVE }

    public companion object {
        public val DEFAULT: Limits = Limits()
    }
}

/** What a task did before handing control back; [value] is the result when [kind] is DONE, else the awaited handle. */
public data class TaskOutcome(val kind: Kind, val task: Int, val value: Value) {
    public enum class Kind { DONE, WAIT, AT_ENTER, AT_LEAVE, YIELD }

    val done: Boolean get() = kind == Kind.DONE
}
