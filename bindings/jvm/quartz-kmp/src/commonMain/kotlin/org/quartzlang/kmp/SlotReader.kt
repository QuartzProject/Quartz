package org.quartzlang.kmp

/** Reads Quartz values out of slots by plain recursion, or level by level on a [Walk] when they nest deep. */
internal class SlotReader private constructor(private val readers: Slots.Readers) {
    companion object {
        fun decode(type: Type, slots: LongArray, at: Int, readers: Slots.Readers): Any? {
            val reader = SlotReader(readers)
            try {
                return reader.direct(type, slots, at, 0)
            } catch (_: Walk.TooDeep) {
                // Its partial result is dropped: the walk reads again from the top.
            }
            return Walk.run(reader.step(type, slots, at))
        }
    }

    private fun direct(type: Type, slots: LongArray, at: Int, depth: Int): Any? {
        var t = type
        var a = at
        while (t.kind == Kind.NULLABLE) {
            if (slots[a] == 0L) {
                return null
            }
            if (!t.isPointerNullable) {
                a += 1
            }
            t = t.element!!
        }
        return when (t.kind) {
            Kind.VOID -> null
            Kind.STRUCT, Kind.TUPLE -> build(t, null, directFields(t.fields, slots, a, depth))
            Kind.ENUM -> {
                val c = variant(t, slots[a])
                build(t, c, directFields(c.payload, slots, a, depth))
            }
            Kind.LIST -> directList(t, slots[a], deeper(depth))
            Kind.MAP -> directMap(t, slots[a], deeper(depth))
            else -> Slots.box(t, slots[a], readers)
        }
    }

    private fun directList(type: Type, bits: Long, depth: Int): List<Any?> {
        val e = type.element!!
        val cell = LongArray(maxOf(1, e.slots))
        val flat = flat(e)
        return List(readers.listLength(bits)) { i ->
            readers.listGet(type, bits, i, cell)
            if (flat) Slots.box(e, cell[0], readers) else direct(e, cell, 0, depth)
        }
    }

    private fun directMap(type: Type, bits: Long, depth: Int): Map<Any?, Any?> {
        val key = LongArray(maxOf(1, type.key!!.slots))
        val value = LongArray(maxOf(1, type.element!!.slots))
        val out = LinkedHashMap<Any?, Any?>()
        for (i in 0 until readers.mapLength(bits)) {
            readers.mapEntry(type, bits, i, key, value)
            val k = direct(type.key!!, key, 0, depth)
            out[k] = direct(type.element!!, value, 0, depth)
        }
        return out
    }

    private fun directFields(fields: List<Type.Field>, slots: LongArray, at: Int, depth: Int): List<Any?> =
        fields.map { f ->
            if (f.box == null) {
                direct(f.type, slots, at + f.offset, depth)
            } else {
                direct(f.type, unbox(f, slots[at + f.offset]), 0, deeper(depth))
            }
        }

    /** The slots of the one value a recursive payload's box holds. */
    private fun unbox(f: Type.Field, bits: Long): LongArray {
        val n = readers.listLength(bits)
        if (n != 1) {
            throw QuartzException("a recursive payload holds $n values instead of one")
        }
        val one = LongArray(maxOf(1, f.type.slots))
        readers.listGet(f.box!!, bits, 0, one)
        return one
    }

    private fun step(type: Type, slots: LongArray, at: Int): Any? {
        var t = type
        var a = at
        while (t.kind == Kind.NULLABLE) {
            if (slots[a] == 0L) {
                return null
            }
            if (!t.isPointerNullable) {
                a += 1
            }
            t = t.element!!
        }
        return when (t.kind) {
            Kind.VOID -> null
            Kind.STRUCT, Kind.TUPLE -> Fields(t, null, t.fields, slots, a)
            Kind.ENUM -> {
                val c = variant(t, slots[a])
                Fields(t, c, c.payload, slots, a)
            }
            Kind.LIST -> ListNode(t, slots[a])
            Kind.MAP -> MapNode(t, slots[a])
            else -> Slots.box(t, slots[a], readers)
        }
    }

    /** The fields of a struct or tuple, or the payload of a variant, all in the same slots. */
    private inner class Fields(
        private val type: Type,
        private val variant: Type.Case?,
        private val fields: List<Type.Field>,
        private val slots: LongArray,
        private val at: Int,
    ) : Walk.Node() {
        private val values = arrayOfNulls<Any?>(fields.size)

        override val size: Int get() = values.size

        override fun part(i: Int): Any? {
            val f = fields[i]
            return if (f.box == null) {
                step(f.type, slots, at + f.offset)
            } else {
                step(f.type, unbox(f, slots[at + f.offset]), 0)
            }
        }

        override fun accept(i: Int, value: Any?) {
            values[i] = value
        }

        override fun finish(): Any? = build(type, variant, values.asList())
    }

    private inner class ListNode(private val type: Type, private val bits: Long) : Walk.Node() {
        private val element = type.element!!
        private val cell = LongArray(maxOf(1, element.slots))
        private val count = readers.listLength(bits)
        private val out = ArrayList<Any?>(count)

        override val size: Int get() = count

        override fun part(i: Int): Any? {
            readers.listGet(type, bits, i, cell)
            return if (flat(element)) Slots.box(element, cell[0], readers) else step(element, cell, 0)
        }

        override fun accept(i: Int, value: Any?) {
            out.add(value)
        }

        override fun finish(): Any? = out
    }

    /** Entry `i` is parts `2i` (its key) and `2i + 1` (its value). */
    private inner class MapNode(private val type: Type, private val bits: Long) : Walk.Node() {
        private val keyCell = LongArray(maxOf(1, type.key!!.slots))
        private val valueCell = LongArray(maxOf(1, type.element!!.slots))
        private val out = LinkedHashMap<Any?, Any?>()
        private val count = (2 * readers.mapLength(bits)).toInt()
        private var pendingKey: Any? = null

        override val size: Int get() = count

        override fun part(i: Int): Any? {
            if (i % 2 == 1) {
                return step(type.element!!, valueCell, 0)
            }
            readers.mapEntry(type, bits, (i / 2).toLong(), keyCell, valueCell)
            return step(type.key!!, keyCell, 0)
        }

        override fun accept(i: Int, value: Any?) {
            if (i % 2 == 0) {
                pendingKey = value
            } else {
                out[pendingKey] = value
            }
        }

        override fun finish(): Any? = out
    }
}

private fun deeper(depth: Int): Int {
    if (depth + 1 >= Walk.DIRECT) {
        throw Walk.TooDeep
    }
    return depth + 1
}

private fun variant(type: Type, index: Long): Type.Case {
    if (index < 0 || index >= type.cases.size) {
        throw QuartzException("enum value with an unknown variant $index")
    }
    return type.cases[index.toInt()]
}

private fun build(type: Type, variant: Type.Case?, values: List<Any?>): Any? = when {
    variant == null && type.kind == Kind.STRUCT -> Struct(type.fields.map { it.name }, values, type.name)
    variant == null -> values
    !type.isResult -> Variant(variant.name, values)
    variant.index == 0 -> Result.Ok(values[0])
    else -> Result.Err(values[0])
}

private fun flat(element: Type): Boolean =
    element.slots == 1 && !element.isComposite && element.kind != Kind.LIST && element.kind != Kind.MAP
