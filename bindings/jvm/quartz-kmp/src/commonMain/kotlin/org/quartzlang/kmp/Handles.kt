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
public fun QuartzValues.from(value: Any?, type: Type, handles: HandleMapper? = null): Any? {
    val v = if (value is Value) value.get() else value
    val kind = type.kind
    if (v == null) {
        require(kind == Kind.NULLABLE || kind == Kind.VOID) { "null cannot be passed as ${describe(type)}" }
        return null
    }
    if (kind.isInteger) {
        return Slots.integer(kind, v)
    }
    return when (kind) {
        Kind.VOID -> null
        Kind.BOOL -> v as? Boolean ?: throw Slots.mismatch(type, v)
        Kind.F64 -> Slots.floating(type, v)
        Kind.F32 -> Slots.floating(type, v).toFloat()
        Kind.CHAR -> Slots.codePoint(type, v).toInt()
        Kind.STR -> v as? String ?: throw Slots.mismatch(type, v)
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
        Kind.LIST -> when (v) {
            is List<*> -> v
            is Array<*> -> v.asList()
            else -> throw Slots.mismatch(type, v)
        }.map { from(it, type.element!!, handles) }
        Kind.MAP -> (v as? Map<*, *> ?: throw Slots.mismatch(type, v)).entries.associateTo(LinkedHashMap()) {
            from(it.key, type.key!!, handles) to from(it.value, type.element!!, handles)
        }
        Kind.STRUCT -> {
            val values = Slots.structValues(type, v)
            Struct(type.fields.map { it.name }, type.fields.mapIndexed { i, f -> from(values[i], f.type, handles) },
                type.name)
        }
        Kind.TUPLE -> Slots.tupleValues(type, v).mapIndexed { i, e -> from(e, type.fields[i].type, handles) }
        Kind.ENUM -> {
            val variant = Slots.variantOf(type, v)
            val case = type.findCase(variant.name) ?: throw IllegalArgumentException(
                "no variant ${variant.name} in ${describe(type)}; it has ${type.cases.map { it.name }}",
            )
            require(case.payload.size == variant.values.size) {
                "variant ${case.name} takes ${case.payload.size} values, got ${variant.values.size}"
            }
            val payload = case.payload.mapIndexed { i, f -> from(variant.values[i], f.type, handles) }
            when {
                !type.isResult -> Variant(case.name, payload)
                case.index == 0 -> Result.Ok(payload[0])
                else -> Result.Err(payload[0])
            }
        }
        Kind.NULLABLE -> from(v, type.element!!, handles)
        else -> v
    }
}

/** [value], as [Value.get] gives it for [type], with every handle id inside replaced by its host object. */
public fun QuartzValues.resolveHandles(value: Any?, type: Type, handles: HandleMapper): Any? {
    if (value == null || !type.holdsHandles) {
        return value
    }
    return when (type.kind) {
        Kind.HANDLE -> handles.toObject((value as? Number ?: return value).toLong(), type)
        Kind.NULLABLE -> resolveHandles(value, type.element!!, handles)
        Kind.LIST -> (value as? List<*> ?: return value).map { resolveHandles(it, type.element!!, handles) }
        Kind.MAP -> (value as? Map<*, *> ?: return value).entries.associateTo(LinkedHashMap()) {
            resolveHandles(it.key, type.key!!, handles) to resolveHandles(it.value, type.element!!, handles)
        }
        Kind.STRUCT -> {
            val s = value as? Struct ?: return value
            Struct(s.names, s.names.mapIndexed { i, n ->
                val field = type.fields.firstOrNull { it.name == n }
                if (field == null) s.values[i] else resolveHandles(s.values[i], field.type, handles)
            }, s.typeName)
        }
        Kind.TUPLE -> (value as? List<*> ?: return value).mapIndexed { i, e ->
            resolveHandles(e, type.fields[i].type, handles)
        }
        Kind.ENUM -> when (value) {
            is Result.Ok<*> -> Result.Ok(resolveHandles(value.value, type.cases[0].payload[0].type, handles))
            is Result.Err<*> -> Result.Err(resolveHandles(value.error, type.cases[1].payload[0].type, handles))
            is Variant -> {
                val case = type.findCase(value.name) ?: return value
                Variant(value.name, value.values.mapIndexed { i, e ->
                    case.payload.getOrNull(i)?.let { resolveHandles(e, it.type, handles) } ?: e
                })
            }
            else -> value
        }
        else -> value
    }
}

private fun describe(type: Type): String = type.name.ifEmpty { type.toString() }
