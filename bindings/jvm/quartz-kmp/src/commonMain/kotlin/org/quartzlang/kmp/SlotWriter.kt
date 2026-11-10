package org.quartzlang.kmp

/** Writes Kotlin values into slots; lists, maps and recursive payloads are built on a [Walk], elements first. */
internal class SlotWriter private constructor(private val makers: Slots.Makers) {
    /** A list, map or one-element box still to build, and the slot that takes it. */
    private class Job(val type: Type, val value: Any, val out: LongArray, val at: Int)

    companion object {
        fun encode(type: Type, value: Any?, out: LongArray, at: Int, makers: Slots.Makers) {
            val w = SlotWriter(makers)
            val jobs = w.inline(type, value, out, at, null)
            if (jobs != null) {
                Walk.run(w.Jobs(jobs))
            }
        }

        private fun add(jobs: MutableList<Job>?, job: Job): MutableList<Job> =
            (jobs ?: ArrayList(2)).also { it.add(job) }
    }

    private fun inline(type: Type, value: Any?, out: LongArray, at: Int, jobs: MutableList<Job>?): MutableList<Job>? {
        if (value is Value) {
            makers.passBack(type, value, out, at)
            return jobs
        }
        return when (type.kind) {
            Kind.VOID -> jobs
            Kind.STRUCT -> fill(type.fields, Slots.structValues(type, value), out, at, jobs)
            Kind.TUPLE -> fill(type.fields, Slots.tupleValues(type, value), out, at, jobs)
            Kind.ENUM -> variant(type, value, out, at, jobs)
            Kind.NULLABLE -> nullable(type, value, out, at, jobs)
            Kind.LIST, Kind.MAP -> add(jobs, job(type, value, out, at))
            else -> {
                out[at] = Slots.scalar(type, value, makers)
                jobs
            }
        }
    }

    private fun variant(type: Type, value: Any?, out: LongArray, at: Int, jobs: MutableList<Job>?): MutableList<Job>? {
        val v = Slots.variantOf(type, value)
        val c = type.findCase(v.name) ?: throw IllegalArgumentException(
            "no variant ${v.name} in $type; it has ${type.cases.map { it.name }}",
        )
        require(c.payload.size == v.values.size) {
            "variant ${c.name} takes ${c.payload.size} values, got ${v.values.size}"
        }
        out.fill(0L, at, at + type.slots)
        out[at] = c.index.toLong()
        return fill(c.payload, v.values, out, at, jobs)
    }

    private fun nullable(type: Type, value: Any?, out: LongArray, at: Int, jobs: MutableList<Job>?): MutableList<Job>? {
        out.fill(0L, at, at + type.slots)
        return when {
            value == null -> jobs
            type.isPointerNullable -> inline(type.element!!, value, out, at, jobs)
            else -> {
                out[at] = 1
                inline(type.element!!, value, out, at + 1, jobs)
            }
        }
    }

    private fun job(type: Type, value: Any?, out: LongArray, at: Int): Job = when {
        value == null -> throw IllegalArgumentException("null cannot be passed as $type")
        type.kind == Kind.LIST && value is List<*> -> Job(type, value, out, at)
        type.kind == Kind.LIST && value is Array<*> -> Job(type, value.asList(), out, at)
        type.kind == Kind.MAP && value is Map<*, *> -> Job(type, value, out, at)
        else -> throw Slots.mismatch(type, value)
    }

    private fun fill(
        fields: List<Type.Field>,
        values: List<Any?>,
        out: LongArray,
        at: Int,
        jobs: MutableList<Job>?,
    ): MutableList<Job>? {
        var j = jobs
        for (i in values.indices) {
            val f = fields[i]
            val box = f.box
            j = if (box != null) {
                add(j, Job(box, listOf(values[i]), out, at + f.offset))
            } else {
                inline(f.type, values[i], out, at + f.offset, j)
            }
        }
        return j
    }

    /** Builds each job's list or map and stores it in the job's slot. */
    private inner class Jobs(private val jobs: List<Job>) : Walk.Node() {
        override val size: Int get() = jobs.size

        override fun part(i: Int): Any? {
            val job = jobs[i]
            return if (job.type.kind == Kind.LIST) {
                ListMaker(job.type, job.value as List<*>)
            } else {
                MapMaker(job.type, job.value as Map<*, *>)
            }
        }

        override fun accept(i: Int, value: Any?) {
            val job = jobs[i]
            job.out[job.at] = value as Long
        }

        override fun finish(): Any? = null
    }

    private inner class ListMaker(private val type: Type, list: List<*>) : Walk.Node() {
        private val element = type.element!!
        private val width = element.slots
        private val count = list.size
        private val elements = list.iterator()
        private val slots = LongArray(maxOf(1, count * width))
        private val eager = !element.holdsContainers

        init {
            if (eager) {
                for (i in 0 until count) {
                    inline(element, elements.next(), slots, i * width, null)
                }
            }
        }

        override val size: Int get() = if (eager) 0 else count

        override fun part(i: Int): Any? = inline(element, elements.next(), slots, i * width, null)?.let { Jobs(it) }

        override fun accept(i: Int, value: Any?) {}

        override fun finish(): Any? = makers.list(type, slots, count)
    }

    private inner class MapMaker(private val type: Type, map: Map<*, *>) : Walk.Node() {
        private val keyWidth = type.key!!.slots
        private val valueWidth = type.element!!.slots
        private val count = map.size
        private val entries = map.entries.iterator()
        private val keys = LongArray(maxOf(1, count * keyWidth))
        private val values = LongArray(maxOf(1, count * valueWidth))
        private val eager = !type.key!!.holdsContainers && !type.element!!.holdsContainers

        init {
            if (eager) {
                for (i in 0 until count) {
                    part(i)
                }
            }
        }

        override val size: Int get() = if (eager) 0 else count

        override fun part(i: Int): Any? {
            val e = entries.next()
            val jobs = inline(type.key!!, e.key, keys, i * keyWidth, null)
            return inline(type.element!!, e.value, values, i * valueWidth, jobs)?.let { Jobs(it) }
        }

        override fun accept(i: Int, value: Any?) {}

        override fun finish(): Any? = makers.map(type, keys, values, count)
    }
}
