package org.quartzlang.jvm;

import java.util.ArrayDeque;
import java.util.function.IntFunction;

/** Runs a conversion of nested values on a heap stack, so a value a million levels deep converts like a flat one. */
final class Walk {
    /** Past this many open levels a value is taken as cyclic; the runtime itself copies a million (D192). */
    static final int MAX_DEPTH = 1 << 22;
    /** Levels converted by plain recursion, which is faster, before a conversion starts again on the walk. */
    static final int DIRECT = 128;
    static final int ON_WALK = -1;

    /** Thrown once plain recursion passes {@link #DIRECT} levels; it carries no stack trace. */
    static final class TooDeep extends RuntimeException {
        private static final long serialVersionUID = 1L;
        static final TooDeep INSTANCE = new TooDeep();

        private TooDeep() {
            super(null, null, false, false);
        }
    }

    /** A value whose parts convert first; {@link #finish()} then builds it from them. */
    abstract static class Node {
        int next;

        /** How many parts it has, read again after each one. */
        abstract int size();

        /** Part {@code i} converted, or a Node whose own parts convert before it. */
        abstract Object part(int i) throws Throwable;

        abstract void accept(int i, Object value);

        /** The value built from the parts, or a Node that converts it further in this node's place. */
        abstract Object finish() throws Throwable;
    }

    /** A Node whose parts and result come from two functions. */
    static final class Parts extends Node {
        interface Part {
            Object step(int i);
        }

        interface Build {
            Object make(Object[] values);
        }

        private final Object[] values;
        private final Part part;
        private final Build build;

        Parts(int size, Part part, Build build) {
            this.values = new Object[size];
            this.part = part;
            this.build = build;
        }

        @Override
        int size() {
            return values.length;
        }

        @Override
        Object part(int i) {
            return part.step(i);
        }

        @Override
        void accept(int i, Object value) {
            values[i] = value;
        }

        @Override
        Object finish() {
            return build.make(values);
        }
    }

    private Walk() {}

    /** A value made of {@code size} parts: built right away at a depth from 0, or as a node on the walk. */
    static Object parts(int depth, int size, Parts.Part part, Parts.Build build) {
        if (depth < 0) {
            return new Parts(size, part, build);
        }
        if (depth >= DIRECT) {
            throw TooDeep.INSTANCE;
        }
        Object[] values = new Object[size];
        for (int i = 0; i < size; ++i) {
            values[i] = part.step(i);
        }
        return build.make(values);
    }

    static int deeper(int depth) {
        return depth < 0 ? ON_WALK : depth + 1;
    }

    /** {@code start} at depth 0 by plain recursion, or again on the walk when it nests deeper than {@link #DIRECT}. */
    static Object convert(IntFunction<Object> start, boolean direct) {
        if (direct) {
            try {
                return start.apply(0);
            } catch (TooDeep deep) {
                // Its partial result is dropped: the walk converts again from the top.
            }
        }
        return value(start.apply(ON_WALK));
    }

    /** {@code first} when it is already a value; otherwise the value its Node builds. */
    static Object run(Object first) throws Throwable {
        if (!(first instanceof Node top)) {
            return first;
        }
        ArrayDeque<Node> stack = null;
        while (true) {
            if (top.next < top.size()) {
                Object r = top.part(top.next);
                if (r instanceof Node child) {
                    if (stack == null) {
                        stack = new ArrayDeque<>();
                    } else if (stack.size() >= MAX_DEPTH) {
                        throw new QuartzException("the value is nested more than " + MAX_DEPTH + " levels deep; "
                                + "is it cyclic?");
                    }
                    stack.push(top);
                    top = child;
                } else {
                    top.accept(top.next++, r);
                }
                continue;
            }
            Object done = top.finish();
            if (done instanceof Node next) {
                top = next;
                continue;
            }
            if (stack == null || stack.isEmpty()) {
                return done;
            }
            top = stack.pop();
            top.accept(top.next++, done);
        }
    }

    static Object value(Object first) {
        try {
            return run(first);
        } catch (Throwable t) {
            throw Native.rethrow(t);
        }
    }
}
