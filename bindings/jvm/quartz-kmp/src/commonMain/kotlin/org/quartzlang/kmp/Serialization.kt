@file:OptIn(ExperimentalSerializationApi::class)

package org.quartzlang.kmp

import kotlinx.serialization.DeserializationStrategy
import kotlinx.serialization.ExperimentalSerializationApi
import kotlinx.serialization.KSerializer
import kotlinx.serialization.SerializationException
import kotlinx.serialization.SerializationStrategy
import kotlinx.serialization.builtins.ByteArraySerializer
import kotlinx.serialization.descriptors.PolymorphicKind
import kotlinx.serialization.descriptors.SerialDescriptor
import kotlinx.serialization.descriptors.StructureKind
import kotlinx.serialization.descriptors.buildClassSerialDescriptor
import kotlinx.serialization.encoding.CompositeDecoder
import kotlinx.serialization.encoding.CompositeEncoder
import kotlinx.serialization.encoding.Decoder
import kotlinx.serialization.encoding.Encoder
import kotlinx.serialization.encoding.decodeStructure
import kotlinx.serialization.encoding.encodeStructure
import kotlinx.serialization.modules.EmptySerializersModule
import kotlinx.serialization.modules.SerializersModule
import kotlinx.serialization.serializer

/**
 * Converts `@Serializable` classes to and from the values a sandbox takes and gives: a class is a [Struct] (by field
 * name) or a tuple (by position), an enum class or a sealed class's subclass is a [Variant] named like it.
 */
public object QuartzValues {
    public fun <T> encode(serializer: SerializationStrategy<T>, value: T): Any? {
        var out: Any? = null
        TreeEncoder { out = it }.encodeSerializableValue(serializer, value)
        return out
    }

    public fun <T> decode(deserializer: DeserializationStrategy<T>, value: Any?): T =
        TreeDecoder(value).decodeSerializableValue(deserializer)
}

/** [value] as a value a sandbox takes, through its serializer: pass it to `call`, `resume` or return it from a host. */
public inline fun <reified T> quartzValue(value: T): Any? = QuartzValues.encode(serializer<T>(), value)

/** The value decoded as a `@Serializable` [T] (a data class by field name, an enum, a sealed class, a list...). */
public inline fun <reified T> Value.decode(): T = QuartzValues.decode(serializer<T>(), get())

/** Argument [index] decoded as a `@Serializable` [T]. */
public inline fun <reified T> HostCall.decode(index: Int): T = QuartzValues.decode(serializer<T>(), arg(index))

/** The serializer of [Result]: Quartz's `Result<T, E>`, or an object with one of `ok` and `err` in other formats. */
public class ResultSerializer<T, E>(private val ok: KSerializer<T>, private val err: KSerializer<E>) :
    KSerializer<Result<T, E>> {
    override val descriptor: SerialDescriptor = buildClassSerialDescriptor("org.quartzlang.kmp.Result") {
        element("ok", ok.descriptor, isOptional = true)
        element("err", err.descriptor, isOptional = true)
    }

    override fun serialize(encoder: Encoder, value: Result<T, E>) {
        if (encoder is TreeEncoder) {
            encoder.sink(
                when (value) {
                    is Result.Ok -> Result.Ok(QuartzValues.encode(ok, value.value))
                    is Result.Err -> Result.Err(QuartzValues.encode(err, value.error))
                },
            )
            return
        }
        encoder.encodeStructure(descriptor) {
            when (value) {
                is Result.Ok -> encodeSerializableElement(descriptor, 0, ok, value.value)
                is Result.Err -> encodeSerializableElement(descriptor, 1, err, value.error)
            }
        }
    }

    override fun deserialize(decoder: Decoder): Result<T, E> {
        if (decoder is TreeDecoder) {
            return when (val v = decoder.value) {
                is Result.Ok<*> -> Result.Ok(QuartzValues.decode(ok, v.value))
                is Result.Err<*> -> Result.Err(QuartzValues.decode(err, v.error))
                is Variant -> when {
                    v.name == "Ok" && v.values.size == 1 -> Result.Ok(QuartzValues.decode(ok, v.values[0]))
                    v.name == "Err" && v.values.size == 1 -> Result.Err(QuartzValues.decode(err, v.values[0]))
                    else -> throw SerializationException("variant ${v.name} is not Ok or Err")
                }
                else -> throw SerializationException("${describe(v)} is not a Result")
            }
        }
        return decoder.decodeStructure(descriptor) {
            var out: Result<T, E>? = null
            while (true) {
                when (val i = decodeElementIndex(descriptor)) {
                    0 -> out = Result.Ok(decodeSerializableElement(descriptor, 0, ok))
                    1 -> out = Result.Err(decodeSerializableElement(descriptor, 1, err))
                    CompositeDecoder.DECODE_DONE -> break
                    else -> throw SerializationException("unexpected element $i of a Result")
                }
            }
            out ?: throw SerializationException("a Result needs ok or err")
        }
    }
}

private val bytesDescriptor = ByteArraySerializer().descriptor

private fun describe(value: Any?): String = when (value) {
    null -> "null"
    is Struct -> "struct ${value.names}"
    is Variant -> "variant ${value.name}"
    else -> "${value::class.simpleName} $value"
}

/** The variant name a sealed subclass's serial name maps to: its last segment. */
private fun simpleName(serialName: String): String = serialName.substringAfterLast('.').substringAfterLast('$')

/** Builds the value tree a serializer describes, handing the result to [sink]. */
internal class TreeEncoder(val sink: (Any?) -> Unit) : Encoder {
    override val serializersModule: SerializersModule = EmptySerializersModule()

    override fun encodeBoolean(value: Boolean) = sink(value)
    override fun encodeByte(value: Byte) = sink(value.toLong())
    override fun encodeShort(value: Short) = sink(value.toLong())
    override fun encodeInt(value: Int) = sink(value.toLong())
    override fun encodeLong(value: Long) = sink(value)
    override fun encodeFloat(value: Float) = sink(value)
    override fun encodeDouble(value: Double) = sink(value)
    override fun encodeChar(value: Char) = sink(value)
    override fun encodeString(value: String) = sink(value)
    override fun encodeNull() = sink(null)
    override fun encodeInline(descriptor: SerialDescriptor): Encoder = this
    override fun encodeEnum(enumDescriptor: SerialDescriptor, index: Int) =
        sink(Variant(enumDescriptor.getElementName(index)))

    override fun <T> encodeSerializableValue(serializer: SerializationStrategy<T>, value: T) {
        if (value is ByteArray && serializer.descriptor == bytesDescriptor) {
            sink(value.copyOf())
        } else {
            serializer.serialize(this, value)
        }
    }

    override fun beginStructure(descriptor: SerialDescriptor): CompositeEncoder = TreeBuilder(descriptor) { values ->
        sink(
            when (descriptor.kind) {
                StructureKind.LIST -> values
                StructureKind.MAP -> LinkedHashMap<Any?, Any?>().also { m ->
                    for (i in values.indices step 2) {
                        m[values[i]] = values[i + 1]
                    }
                }
                is PolymorphicKind -> {
                    val name = simpleName(values[0] as String)
                    when (val payload = values[1]) {
                        is Struct -> Variant(name, payload.values)
                        else -> Variant(name, listOf(payload))
                    }
                }
                else -> Struct(
                    List(descriptor.elementsCount) { descriptor.getElementName(it) },
                    values,
                    simpleName(descriptor.serialName),
                )
            },
        )
    }
}

private class TreeBuilder(private val descriptor: SerialDescriptor, private val done: (List<Any?>) -> Unit) :
    CompositeEncoder {
    private val values = ArrayList<Any?>()
    override val serializersModule: SerializersModule = EmptySerializersModule()

    private fun put(index: Int, value: Any?) {
        while (values.size <= index) {
            values.add(null)
        }
        values[index] = value
    }

    private fun child(index: Int) = TreeEncoder { put(index, it) }

    override fun encodeBooleanElement(descriptor: SerialDescriptor, index: Int, value: Boolean) = put(index, value)
    override fun encodeByteElement(descriptor: SerialDescriptor, index: Int, value: Byte) = put(index, value.toLong())
    override fun encodeShortElement(descriptor: SerialDescriptor, index: Int, value: Short) =
        put(index, value.toLong())

    override fun encodeIntElement(descriptor: SerialDescriptor, index: Int, value: Int) = put(index, value.toLong())
    override fun encodeLongElement(descriptor: SerialDescriptor, index: Int, value: Long) = put(index, value)
    override fun encodeFloatElement(descriptor: SerialDescriptor, index: Int, value: Float) = put(index, value)
    override fun encodeDoubleElement(descriptor: SerialDescriptor, index: Int, value: Double) = put(index, value)
    override fun encodeCharElement(descriptor: SerialDescriptor, index: Int, value: Char) = put(index, value)
    override fun encodeStringElement(descriptor: SerialDescriptor, index: Int, value: String) = put(index, value)
    override fun encodeInlineElement(descriptor: SerialDescriptor, index: Int): Encoder = child(index)

    override fun <T> encodeSerializableElement(
        descriptor: SerialDescriptor,
        index: Int,
        serializer: SerializationStrategy<T>,
        value: T,
    ) = child(index).encodeSerializableValue(serializer, value)

    override fun <T : Any> encodeNullableSerializableElement(
        descriptor: SerialDescriptor,
        index: Int,
        serializer: SerializationStrategy<T>,
        value: T?,
    ) = if (value == null) put(index, null) else child(index).encodeSerializableValue(serializer, value)

    override fun endStructure(descriptor: SerialDescriptor) {
        while (values.size < this.descriptor.elementsCount && this.descriptor.kind !is StructureKind.LIST &&
            this.descriptor.kind !is StructureKind.MAP
        ) {
            values.add(null)
        }
        done(values)
    }
}

/** Reads a value tree as a serializer asks for it. */
internal class TreeDecoder(val value: Any?) : Decoder {
    override val serializersModule: SerializersModule = EmptySerializersModule()

    private fun fail(what: String): Nothing = throw SerializationException("${describe(value)} cannot be read as $what")

    private fun long(what: String): Long = when (value) {
        is Long -> value
        is Int -> value.toLong()
        is Short -> value.toLong()
        is Byte -> value.toLong()
        else -> fail(what)
    }

    private fun double(what: String): Double = when (value) {
        is Double -> value
        is Float -> value.toDouble()
        else -> fail(what)
    }

    override fun decodeNotNullMark(): Boolean = value != null
    override fun decodeNull(): Nothing? = null
    override fun decodeBoolean(): Boolean = value as? Boolean ?: fail("Boolean")
    override fun decodeByte(): Byte = long("Byte").let { if (it == it.toByte().toLong()) it.toByte() else fail("Byte") }
    override fun decodeShort(): Short =
        long("Short").let { if (it == it.toShort().toLong()) it.toShort() else fail("Short") }

    override fun decodeInt(): Int = long("Int").let { if (it == it.toInt().toLong()) it.toInt() else fail("Int") }
    override fun decodeLong(): Long = long("Long")
    override fun decodeFloat(): Float = double("Float").toFloat()
    override fun decodeDouble(): Double = double("Double")
    override fun decodeChar(): Char = when (value) {
        is Char -> value
        is Int -> if (value in 0..0xFFFF) value.toChar() else fail("Char")
        else -> fail("Char")
    }

    override fun decodeString(): String = value as? String ?: fail("String")
    override fun decodeInline(descriptor: SerialDescriptor): Decoder = this

    override fun decodeEnum(enumDescriptor: SerialDescriptor): Int {
        val name = when (value) {
            is String -> value
            is Variant -> if (value.values.isEmpty()) value.name else fail(enumDescriptor.serialName)
            else -> fail(enumDescriptor.serialName)
        }
        val names = List(enumDescriptor.elementsCount) { enumDescriptor.getElementName(it) }
        val index = names.indexOf(name).takeIf { it >= 0 } ?: names.indexOfFirst { normalize(it) == normalize(name) }
        if (index < 0) {
            throw SerializationException("${enumDescriptor.serialName} has no constant for the variant $name")
        }
        return index
    }

    @Suppress("UNCHECKED_CAST")
    override fun <T> decodeSerializableValue(deserializer: DeserializationStrategy<T>): T =
        if (value is ByteArray && deserializer.descriptor == bytesDescriptor) {
            value.copyOf() as T
        } else {
            deserializer.deserialize(this)
        }

    override fun beginStructure(descriptor: SerialDescriptor): CompositeDecoder = when (descriptor.kind) {
        StructureKind.LIST -> TreeReader(positional(value as? List<*> ?: fail("a list")))
        StructureKind.MAP -> {
            val map = value as? Map<*, *> ?: fail("a map")
            TreeReader(map.entries.flatMap { listOf(it.key, it.value) }.withIndex().map { it.index to it.value })
        }
        is PolymorphicKind -> TreeReader(sealed(descriptor))
        else -> TreeReader(product(descriptor))
    }

    private fun positional(values: List<*>): List<Pair<Int, Any?>> = values.mapIndexed { i, v -> i to v }

    private fun product(descriptor: SerialDescriptor): List<Pair<Int, Any?>> {
        val values: List<Any?> = when (value) {
            is Struct -> {
                if (!sameType(value.typeName, simpleName(descriptor.serialName))) {
                    throw SerializationException(
                        "${descriptor.serialName} does not map to the struct ${value.typeName}",
                    )
                }
                return List(descriptor.elementsCount) { i ->
                    val name = descriptor.getElementName(i)
                    val at = value.names.indexOf(name).takeIf { it >= 0 }
                        ?: value.names.indexOfFirst { normalize(it) == normalize(name) }
                    if (at < 0) null else i to value.values[at]
                }.filterNotNull()
            }
            is List<*> -> value
            is Variant -> value.values
            is Result.Ok<*> -> listOf(value.value)
            is Result.Err<*> -> listOf(value.error)
            else -> fail(descriptor.serialName)
        }
        if (values.size != descriptor.elementsCount) {
            throw SerializationException(
                "${descriptor.serialName} has ${descriptor.elementsCount} elements, the value has ${values.size}",
            )
        }
        return positional(values)
    }

    private fun sealed(descriptor: SerialDescriptor): List<Pair<Int, Any?>> {
        val variant = when (value) {
            is Variant -> value
            is Result.Ok<*> -> Variant("Ok", listOf(value.value))
            is Result.Err<*> -> Variant("Err", listOf(value.error))
            else -> fail(descriptor.serialName)
        }
        val subclasses = descriptor.getElementDescriptor(1)
        val names = List(subclasses.elementsCount) { subclasses.getElementName(it) }
        val serialName = names.firstOrNull { simpleName(it) == variant.name }
            ?: names.firstOrNull { normalize(simpleName(it)) == normalize(variant.name) }
            ?: throw SerializationException("no subclass of ${descriptor.serialName} is named like ${variant.name}")
        return listOf(0 to serialName, 1 to variant)
    }
}

private class TreeReader(private val entries: List<Pair<Int, Any?>>) : CompositeDecoder {
    private var next = 0
    private var current: Any? = null
    override val serializersModule: SerializersModule = EmptySerializersModule()

    override fun decodeElementIndex(descriptor: SerialDescriptor): Int {
        if (next >= entries.size) {
            return CompositeDecoder.DECODE_DONE
        }
        val (index, value) = entries[next++]
        current = value
        return index
    }

    private fun child() = TreeDecoder(current)

    override fun decodeBooleanElement(descriptor: SerialDescriptor, index: Int) = child().decodeBoolean()
    override fun decodeByteElement(descriptor: SerialDescriptor, index: Int) = child().decodeByte()
    override fun decodeShortElement(descriptor: SerialDescriptor, index: Int) = child().decodeShort()
    override fun decodeIntElement(descriptor: SerialDescriptor, index: Int) = child().decodeInt()
    override fun decodeLongElement(descriptor: SerialDescriptor, index: Int) = child().decodeLong()
    override fun decodeFloatElement(descriptor: SerialDescriptor, index: Int) = child().decodeFloat()
    override fun decodeDoubleElement(descriptor: SerialDescriptor, index: Int) = child().decodeDouble()
    override fun decodeCharElement(descriptor: SerialDescriptor, index: Int) = child().decodeChar()
    override fun decodeStringElement(descriptor: SerialDescriptor, index: Int) = child().decodeString()
    override fun decodeInlineElement(descriptor: SerialDescriptor, index: Int): Decoder = child()

    override fun <T> decodeSerializableElement(
        descriptor: SerialDescriptor,
        index: Int,
        deserializer: DeserializationStrategy<T>,
        previousValue: T?,
    ): T = child().decodeSerializableValue(deserializer)

    override fun <T : Any> decodeNullableSerializableElement(
        descriptor: SerialDescriptor,
        index: Int,
        deserializer: DeserializationStrategy<T?>,
        previousValue: T?,
    ): T? = if (current == null) null else child().decodeSerializableValue(deserializer)

    override fun endStructure(descriptor: SerialDescriptor) {}
}
