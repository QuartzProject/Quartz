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

/** A type: its kind, the slots it takes (a struct takes the sum of its fields), its id and its parts. */
public class Type(public val kind: Kind, public val slots: Int = 1, public val id: Int = 0, element: Type? = null) {
    /**
     * A field of a struct, an element of a tuple ([name] empty) or a value of a variant, at slot [offset], with its
     * `///` [doc]. A recursive payload has [box], the list of one element its slot holds, and [type] is the value's.
     */
    public data class Field(
        val name: String,
        val type: Type,
        val offset: Int,
        val box: Type? = null,
        val doc: String = "",
    ) {
        /** True for a recursive payload, read and taken as the value itself. */
        val boxed: Boolean get() = box != null
    }

    /** A variant of an enum: its [name], the value of slot 0 ([index]), the values it carries and its `///` [doc]. */
    public data class Case(val name: String, val index: Int, val payload: List<Field>, val doc: String = "")

    /** The element of a list or nullable, the value of a map, or null. */
    public var element: Type? = element
        private set

    /** The key of a map, or null. */
    public var key: Type? = null
        private set

    /** Fields of a struct or elements of a tuple, empty for other kinds. */
    public var fields: List<Field> = emptyList()
        private set

    /** Variants of an enum, empty for other kinds. */
    public var cases: List<Case> = emptyList()
        private set

    private var pointer: Boolean = kind == Kind.NULLABLE && slots == 1

    /** How the source writes it (`Point`, `[int]`, `Box<int>`); empty when unknown. */
    public var name: String = ""
        internal set

    /** The package of a struct, enum or handle type (`org.example`); empty for others and when unknown. */
    public var packageName: String = ""
        internal set

    /** The `///` text of a struct or enum declaration; empty for other types and when it has none. */
    public var doc: String = ""
        internal set

    /** [name] with its package in front, when it has one. */
    public val qualifiedName: String get() = if (packageName.isEmpty()) name else "$packageName.$name"

    /** True when a value of this type can hold a `@handle` anywhere inside, recursive types included. */
    public val holdsHandles: Boolean by lazy { scanHandles(this, mutableSetOf()) }

    /** Filled in once the type is cached, so recursive types resolve. */
    internal fun complete(element: Type?, key: Type?, fields: List<Field>, cases: List<Case>, pointer: Boolean) {
        this.element = element
        this.key = key
        this.fields = fields
        this.cases = cases
        this.pointer = pointer
    }

    /** A nullable that is one slot, 0 when null (str, Bytes, lists, maps); other nullables add a presence slot. */
    public val isPointerNullable: Boolean get() = kind == Kind.NULLABLE && pointer

    /** A one-slot nullable str, Bytes or list, which is null or a pointer. */
    public val isNullableReference: Boolean
        get() = kind == Kind.NULLABLE && slots == 1 && element?.kind in setOf(Kind.STR, Kind.BYTES, Kind.LIST, Kind.MAP)

    /** An enum shaped like `Result<T, E>`: variants `Ok` and `Err` with one value each. */
    public val isResult: Boolean
        get() = kind == Kind.ENUM && cases.size == 2 && cases[0].name == "Ok" && cases[1].name == "Err" &&
            cases[0].payload.size == 1 && cases[1].payload.size == 1

    /** True for structs, tuples, enums and nullables, which the bindings lay out field by field. */
    public val isComposite: Boolean
        get() = kind == Kind.STRUCT || kind == Kind.TUPLE || kind == Kind.ENUM || kind == Kind.NULLABLE

    /** The variant called [name], matched exactly or else ignoring case and underscores. */
    public fun findCase(name: String): Case? =
        cases.firstOrNull { it.name == name } ?: cases.firstOrNull { normalize(it.name) == normalize(name) }

    override fun equals(other: Any?): Boolean =
        other is Type && other.kind == kind && other.slots == slots && other.element == element &&
            other.key == key && other.cases.map { it.name } == cases.map { it.name } &&
            other.fields.map { it.name to it.type.kind } == fields.map { it.name to it.type.kind }

    override fun hashCode(): Int = kind.hashCode() * 31 + slots

    override fun toString(): String {
        val inner = element?.let { "<" + (key?.let { k -> "$k, " } ?: "") + it + ">" } ?: ""
        val name = kind.name.lowercase() + inner
        return if (slots == 1) name else "$name[$slots]"
    }
}

private fun scanHandles(type: Type?, seen: MutableSet<Type>): Boolean {
    if (type == null || seen.any { it === type }) {
        return false
    }
    seen += type
    return type.kind == Kind.HANDLE || scanHandles(type.element, seen) || scanHandles(type.key, seen) ||
        type.fields.any { scanHandles(it.type, seen) } ||
        type.cases.any { c -> c.payload.any { scanHandles(it.type, seen) } }
}

/** `IN_PROGRESS`, `InProgress` and `in_progress` all become `inprogress`. */
internal fun normalize(name: String): String = name.replace("_", "").lowercase()

/** True when two type names fit, generic arguments aside; an unknown (empty) name fits anything. */
internal fun sameType(a: String, b: String): Boolean {
    val x = a.substringBefore('<')
    val y = b.substringBefore('<')
    return x.isEmpty() || y.isEmpty() || x == y || normalize(x) == normalize(y)
}

/** What a caller passes (`state` parameters left out) and what comes back. */
public data class Signature(val params: List<Type>, val result: Type) {
    val arity: Int get() = params.size

    /** Slot offset of parameter [index] in the argument array. */
    public fun offset(index: Int): Int = params.take(index).sumOf { it.slots }

    override fun toString(): String = "$params -> $result"
}

/**
 * A host function a program imports, by its binding name with its package; the runtime binds [builtIn] ones (the
 * standard library), and [required] is false when no function calls it, so `start` does not ask for its binding.
 */
public data class Import(
    val name: String,
    val signature: Signature,
    val builtIn: Boolean = false,
    val packageName: String = "",
    val required: Boolean = true,
) {
    /** The name without its package (`greet`, `Player.health`). */
    val simpleName: String get() = simpleNameOf(name, packageName)
}

internal fun simpleNameOf(name: String, pkg: String): String =
    if (pkg.isEmpty() || !name.startsWith("$pkg.")) name else name.substring(pkg.length + 1)

/** One source file; a name ending in `.qzi` is a host interface. */
public data class Source(val name: String, val text: String)

/**
 * How to compile: [emitBinary] also produces the .qzb module, [manifest] is package.toml text, and [rules] sets warning
 * levels by rule id (`unused-variable`, or `*` for every rule) in order, a later entry winning.
 */
public data class CompileOptions(
    val emitBinary: Boolean = false,
    val keepLines: Boolean = true,
    val manifest: String? = null,
    val rules: Map<String, WarningLevel> = emptyMap(),
) {
    /** Sets the level of [rule] (`*` for every rule), after and so above any level set before. */
    public fun withRule(rule: String, level: WarningLevel): CompileOptions =
        copy(rules = LinkedHashMap(rules).apply { remove(rule); put(rule, level) })

    /** Every warning as an error, like `quartz check --deny-warnings`. */
    public fun denyWarnings(): CompileOptions = withRule("*", WarningLevel.ERROR)
}

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
