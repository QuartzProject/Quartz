package org.quartzlang.kmp

/**
 * One compiler diagnostic. Lines and columns start at 1 and columns count UTF-8 bytes; all four are 0 when it has no
 * place in a source. [rule] is the warning rule id, kept when the warning was raised to an error, and empty otherwise.
 */
public data class Diagnostic(
    val severity: Severity,
    val rule: String,
    val file: String,
    val line: Int,
    val column: Int,
    val endLine: Int,
    val endColumn: Int,
    val message: String,
) {
    /** In the order of `qz_severity`. */
    public enum class Severity {
        ERROR, WARNING, NOTE;

        internal companion object {
            fun of(code: Int): Severity = entries.getOrElse(code) { ERROR }
        }
    }

    val isError: Boolean get() = severity == Severity.ERROR

    /** `file:line:column: severity: message`, the shape editors turn into links. */
    override fun toString(): String {
        val where = if (file.isEmpty()) "" else if (line == 0) "$file: " else "$file:$line:$column: "
        return "$where${severity.name.lowercase()}: $message"
    }
}

/** What the compiler does with a warning rule, in the order of `qz_level`. */
public enum class WarningLevel { IGNORE, WARNING, ERROR }

/** What a function's declaration wrote, in the order of `qz_visibility`. */
public enum class Visibility {
    DEFAULT, PRIVATE, INTERNAL, PUBLIC;

    internal companion object {
        fun of(code: Int): Visibility = entries.getOrElse(code) { DEFAULT }
    }
}

/**
 * An attribute of a function as written (`@command("heal", perm = "x.heal")`, `@on(Join)`): its [name] without the
 * `@`, its [arguments] in source order and, for `@on(..., where = expr)`, the exported [filter] function that tests the
 * handler's first argument.
 */
public data class Attribute(val name: String, val arguments: List<Argument>, val filter: Export?) {
    /** The type of an argument's value, in the order of `qz_attribute_arg_kind`; NAME is a dotted name as text. */
    public enum class ArgumentKind {
        INT, FLOAT, BOOL, STR, NAME;

        internal companion object {
            fun of(code: Int): ArgumentKind = entries.getOrElse(code) { NAME }
        }
    }

    /** One argument: [key] is empty for a positional one; [value] is a Long, Double, Boolean or String by [kind]. */
    public data class Argument(val key: String, val kind: ArgumentKind, val value: Any) {
        val isPositional: Boolean get() = key.isEmpty()

        public fun asLong(): Long = checked(ArgumentKind.INT) as Long

        public fun asDouble(): Double = if (kind == ArgumentKind.INT) asLong().toDouble() else
            checked(ArgumentKind.FLOAT) as Double

        public fun asBoolean(): Boolean = checked(ArgumentKind.BOOL) as Boolean

        /** The text of a STR or NAME argument. */
        public fun asString(): String {
            check(kind == ArgumentKind.STR || kind == ArgumentKind.NAME) { "argument $this is $kind, not text" }
            return value as String
        }

        private fun checked(wanted: ArgumentKind): Any {
            check(kind == wanted) { "argument $this is $kind, not $wanted" }
            return value
        }

        override fun toString(): String {
            val text = if (kind == ArgumentKind.STR) "\"$value\"" else value.toString()
            return if (key.isEmpty()) text else "$key = $text"
        }
    }

    /** Positional argument [index], or null. */
    public fun positional(index: Int): Argument? = arguments.filter { it.isPositional }.getOrNull(index)

    /** The argument named [key], or null. */
    public fun named(key: String): Argument? = arguments.firstOrNull { it.key == key }
}
