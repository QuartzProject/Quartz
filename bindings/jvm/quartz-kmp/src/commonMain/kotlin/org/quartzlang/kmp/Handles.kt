package org.quartzlang.kmp

/** How [QuartzValues.from] and [QuartzValues.resolveHandles] turn `@handle` values into host objects and back. */
public interface HandleMapper {
    /** The host object behind handle [id] of [type]; throw when the id is unknown. */
    public fun toObject(id: Long, type: Type): Any?

    /** The id [obj] travels as, for a handle of [type]. */
    public fun toId(obj: Any, type: Type): Long
}

/**
 * [value] (a [Struct], Map, List, Array, Pair, Triple, [Variant], [Result], enum constant or scalar) as what
 * [Value.get] would give for [type]: a [Struct] named like it, Long for every integer and handle, Int for a char, and
 * so on. With [handles], host objects where a handle goes become ids.
 *
 * @throws IllegalArgumentException when [value] does not fit [type]
 */
public fun QuartzValues.from(value: Any?, type: Type, handles: HandleMapper? = null): Any? =
    Walk.convert(handles == null) { fromStep(value, type, handles, it) }

private fun fromStep(value: Any?, type: Type, handles: HandleMapper?, depth: Int): Any? {
    val v = if (value is Value) value.get() else value
    val kind = type.kind
    if (v == null) {
        require(kind == Kind.NULLABLE || kind == Kind.VOID) { "null cannot be passed as ${describe(type)}" }
        return null
    }
    if (kind.isInteger) {
        return Slots.integer(kind, v)
    }
    val d = Walk.deeper(depth)
    return when (kind) {
        Kind.VOID -> null
        Kind.BOOL -> v as? Boolean ?: throw Slots.mismatch(type, v)
        Kind.F64 -> Slots.floating(type, v)
        Kind.F32 -> Slots.floating(type, v).toFloat()
        Kind.CHAR -> Slots.codePoint(type, v).toInt()
        Kind.STR -> if (v is String || v is LossyText) v else throw Slots.mismatch(type, v)
        Kind.BYTES -> v as? ByteArray ?: throw Slots.mismatch(type, v)
        Kind.HANDLE -> when {
            v is Long -> v
            v is Int -> v.toLong()
            handles != null -> handles.toId(v, type)
            else -> throw IllegalArgumentException(
                "a ${v::class.simpleName} cannot be passed as the handle ${describe(type)}: " +
                    "pass its id or a HandleMapper",
            )
        }
        Kind.LIST -> {
            val items = when (v) {
                is List<*> -> v
                is Array<*> -> v.asList()
                else -> throw Slots.mismatch(type, v)
            }
            Walk.parts(depth, items.size, { fromStep(items[it], type.element!!, handles, d) }) { it.asList() }
        }
        Kind.MAP -> {
            val flat = entries(v as? Map<*, *> ?: throw Slots.mismatch(type, v))
            Walk.parts(depth, flat.size, {
                fromStep(flat[it], if (it % 2 == 0) type.key!! else type.element!!, handles, d)
            }, ::linkedMap)
        }
        Kind.STRUCT -> {
            val values = Slots.structValues(type, v)
            Walk.parts(depth, values.size, { fromStep(values[it], type.fields[it].type, handles, d) }) {
                Struct(type.fields.map { f -> f.name }, it.asList(), type.name)
            }
        }
        Kind.TUPLE -> {
            val values = Slots.tupleValues(type, v)
            Walk.parts(depth, values.size, { fromStep(values[it], type.fields[it].type, handles, d) }) { it.asList() }
        }
        Kind.ENUM -> {
            val variant = Slots.variantOf(type, v)
            val case = type.findCase(variant.name) ?: throw IllegalArgumentException(
                "no variant ${variant.name} in ${describe(type)}; it has ${type.cases.map { it.name }}",
            )
            require(case.payload.size == variant.values.size) {
                "variant ${case.name} takes ${case.payload.size} values, got ${variant.values.size}"
            }
            Walk.parts(depth, case.payload.size, { fromStep(variant.values[it], case.payload[it].type, handles, d) }) {
                when {
                    !type.isResult -> Variant(case.name, it.asList())
                    case.index == 0 -> Result.Ok(it[0])
                    else -> Result.Err(it[0])
                }
            }
        }
        Kind.NULLABLE -> fromStep(v, type.element!!, handles, depth)
        else -> v
    }
}

/** [value], as [Value.get] gives it for [type], with every handle id inside replaced by its host object. */
public fun QuartzValues.resolveHandles(value: Any?, type: Type, handles: HandleMapper): Any? =
    Walk.convert(false) { resolveStep(value, type, handles, it) }

private fun resolveStep(value: Any?, type: Type, handles: HandleMapper, depth: Int): Any? {
    if (value == null || !type.holdsHandles) {
        return value
    }
    val d = Walk.deeper(depth)
    return when (type.kind) {
        Kind.HANDLE -> handles.toObject((value as? Number ?: return value).toLong(), type)
        Kind.NULLABLE -> resolveStep(value, type.element!!, handles, depth)
        Kind.LIST -> {
            val items = value as? List<*> ?: return value
            Walk.parts(depth, items.size, { resolveStep(items[it], type.element!!, handles, d) }) { it.asList() }
        }
        Kind.MAP -> {
            val flat = entries(value as? Map<*, *> ?: return value)
            Walk.parts(depth, flat.size, {
                resolveStep(flat[it], if (it % 2 == 0) type.key!! else type.element!!, handles, d)
            }, ::linkedMap)
        }
        Kind.STRUCT -> {
            val s = value as? Struct ?: return value
            Walk.parts(depth, s.size, { i ->
                val field = type.fields.firstOrNull { it.name == s.names[i] }
                if (field == null) s.values[i] else resolveStep(s.values[i], field.type, handles, d)
            }) { Struct(s.names, it.asList(), s.typeName) }
        }
        Kind.TUPLE -> {
            val items = value as? List<*> ?: return value
            Walk.parts(depth, items.size, { resolveStep(items[it], type.fields[it].type, handles, d) }) { it.asList() }
        }
        Kind.ENUM -> when (value) {
            is Result.Ok<*> -> Walk.parts(depth, 1, {
                resolveStep(value.value, type.cases[0].payload[0].type, handles, d)
            }) { Result.Ok(it[0]) }
            is Result.Err<*> -> Walk.parts(depth, 1, {
                resolveStep(value.error, type.cases[1].payload[0].type, handles, d)
            }) { Result.Err(it[0]) }
            is Variant -> {
                val case = type.findCase(value.name) ?: return value
                Walk.parts(depth, value.values.size, { i ->
                    val f = case.payload.getOrNull(i)
                    val e = value.values[i]
                    if (f == null) e else resolveStep(e, f.type, handles, d) ?: e
                }) { Variant(value.name, it.asList()) }
            }
            else -> value
        }
        else -> value
    }
}

/** A map's keys and values alternating, so [Walk.parts] converts entry `i` as parts `2i` and `2i + 1`. */
internal fun entries(map: Map<*, *>): List<Any?> {
    val out = ArrayList<Any?>(2 * map.size)
    for ((k, v) in map) {
        out.add(k)
        out.add(v)
    }
    return out
}

internal fun linkedMap(flat: Array<Any?>): Map<Any?, Any?> {
    val out = LinkedHashMap<Any?, Any?>()
    for (i in flat.indices step 2) {
        out[flat[i]] = flat[i + 1]
    }
    return out
}

private fun describe(type: Type): String = type.name.ifEmpty { type.toString() }
