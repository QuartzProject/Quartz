package org.quartzlang.kmp

/** Runs a conversion of nested values on a heap stack, so a value a million levels deep converts like a flat one. */
internal object Walk {
    /** Past this many open levels a value is taken as cyclic; the runtime itself copies a million (D192). */
    const val MAX_DEPTH: Int = 1 shl 22

    /** Levels converted by plain recursion, which is faster, before a conversion starts again on the walk. */
    const val DIRECT: Int = 128

    const val ON_WALK: Int = -1

    /** Thrown once plain recursion passes [DIRECT] levels. */
    object TooDeep : RuntimeException("nested past the direct conversion depth")

    /** A value whose parts convert first; [finish] then builds it from them. */
    abstract class Node {
        var next: Int = 0

        /** How many parts it has, read again after each one. */
        abstract val size: Int

        /** Part [i] converted, or a Node whose own parts convert before it. */
        abstract fun part(i: Int): Any?

        abstract fun accept(i: Int, value: Any?)

        /** The value built from the parts, or a Node that converts it further in this node's place. */
        abstract fun finish(): Any?
    }

    /** A Node whose parts and result come from two functions. */
    class Parts(size: Int, private val step: (Int) -> Any?, private val build: (Array<Any?>) -> Any?) : Node() {
        private val values = arrayOfNulls<Any?>(size)

        override val size: Int get() = values.size

        override fun part(i: Int): Any? = step(i)

        override fun accept(i: Int, value: Any?) {
            values[i] = value
        }

        override fun finish(): Any? = build(values)
    }

    /** A value made of [size] parts: built right away at a depth from 0, or as a node on the walk. */
    fun parts(depth: Int, size: Int, step: (Int) -> Any?, build: (Array<Any?>) -> Any?): Any? {
        if (depth < 0) {
            return Parts(size, step, build)
        }
        if (depth >= DIRECT) {
            throw TooDeep
        }
        return build(Array(size) { step(it) })
    }

    fun deeper(depth: Int): Int = if (depth < 0) ON_WALK else depth + 1

    /** [start] at depth 0 by plain recursion, or again on the walk when it nests deeper than [DIRECT]. */
    fun convert(direct: Boolean, start: (Int) -> Any?): Any? {
        if (direct) {
            try {
                return start(0)
            } catch (_: TooDeep) {
                // Its partial result is dropped: the walk converts again from the top.
            }
        }
        return run(start(ON_WALK))
    }

    /** [first] when it is already a value; otherwise the value its Node builds. */
    fun run(first: Any?): Any? {
        var top = first as? Node ?: return first
        val stack = ArrayList<Node>()
        while (true) {
            if (top.next < top.size) {
                val r = top.part(top.next)
                if (r is Node) {
                    if (stack.size >= MAX_DEPTH) {
                        throw QuartzException("the value is nested more than $MAX_DEPTH levels deep; is it cyclic?")
                    }
                    stack.add(top)
                    top = r
                } else {
                    top.accept(top.next++, r)
                }
                continue
            }
            val done = top.finish()
            if (done is Node) {
                top = done
                continue
            }
            if (stack.isEmpty()) {
                return done
            }
            top = stack.removeAt(stack.size - 1)
            top.accept(top.next++, done)
        }
    }
}
