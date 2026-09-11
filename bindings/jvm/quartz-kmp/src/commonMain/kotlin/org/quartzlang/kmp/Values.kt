package org.quartzlang.kmp

import kotlinx.serialization.Serializable

/**
 * A Quartz struct copied out of a sandbox, or built to pass one: its fields by name, in declaration order. A non-empty
 * [typeName] is checked against the struct it is passed as.
 */
public class Struct(
    public val names: List<String>,
    public val values: List<Any?>,
    public val typeName: String = "",
) {
    init {
        require(names.size == values.size) { "${names.size} field names for ${values.size} values" }
    }

    public constructor(vararg fields: Pair<String, Any?>) : this(fields.map { it.first }, fields.map { it.second })

    public val size: Int get() = values.size

    public operator fun get(index: Int): Any? = values[index]

    /** @throws NoSuchElementException when the struct has no field called [name] */
    public operator fun get(name: String): Any? {
        val i = names.indexOf(name)
        if (i < 0) {
            throw NoSuchElementException("the struct has no field $name; it has $names")
        }
        return values[i]
    }

    public fun has(name: String): Boolean = name in names

    public fun toMap(): Map<String, Any?> = names.zip(values).toMap()

    override fun equals(other: Any?): Boolean =
        other is Struct && other.names == names && other.values.size == values.size &&
            other.values.indices.all { same(other.values[it], values[it]) }

    override fun hashCode(): Int =
        names.hashCode() * 31 + values.sumOf { (it as? ByteArray)?.contentHashCode() ?: it.hashCode() }

    override fun toString(): String = names.indices.joinToString(", ", "{", "}") { "${names[it]}=${show(values[it])}" }
}

/** A value of a Quartz enum: the variant [name] and the values it carries, in order. */
public data class Variant(val name: String, val values: List<Any?> = emptyList()) {
    public operator fun get(index: Int): Any? = values[index]

    override fun toString(): String =
        if (values.isEmpty()) name else values.joinToString(", ", "$name(", ")", transform = ::show)

    public companion object {
        /** A variant built by name with its values, as an argument or a host function's result. */
        public fun of(name: String, vararg values: Any?): Variant = Variant(name, values.toList())
    }
}

/** A Quartz `Result<T, E>`: a host function returns [Ok] or [Err], and a result reads as one. */
@Serializable(with = ResultSerializer::class)
public sealed interface Result<out T, out E> {
    public data class Ok<out T>(val value: T) : Result<T, Nothing>

    public data class Err<out E>(val error: E) : Result<Nothing, E>

    public val isOk: Boolean get() = this is Ok

    /** The success value, or [ResultException] carrying the error. */
    public fun getOrThrow(): T = when (this) {
        is Ok -> value
        is Err -> throw ResultException(error)
    }
}

/** A Quartz `Err` turned into an exception by [Result.getOrThrow]; not a trap. */
public class ResultException(public val error: Any?) : QuartzException(error.toString())

private fun same(a: Any?, b: Any?): Boolean = if (a is ByteArray && b is ByteArray) a.contentEquals(b) else a == b

private fun show(value: Any?): String = if (value is ByteArray) value.contentToString() else value.toString()
