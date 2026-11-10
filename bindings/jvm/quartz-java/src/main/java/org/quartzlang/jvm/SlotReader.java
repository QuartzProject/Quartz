package org.quartzlang.jvm;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** Reads Quartz values out of slots by plain recursion, or level by level on a {@link Walk} when they nest deep. */
final class SlotReader {
    private final Slots.Readers readers;

    private SlotReader(Slots.Readers readers) {
        this.readers = readers;
    }

    static Object decode(Type type, long[] slots, int at, Slots.Readers readers) {
        SlotReader reader = new SlotReader(readers);
        try {
            return reader.direct(type, slots, at, 0);
        } catch (Walk.TooDeep deep) {
            // Its partial result is dropped: the walk reads again from the top.
        }
        return Walk.value(reader.step(type, slots, at));
    }

    private Object direct(Type type, long[] slots, int at, int depth) {
        while (type.kind() == Kind.NULLABLE) {
            if (slots[at] == 0) {
                return null;
            }
            at = type.isPointerNullable() ? at : at + 1;
            type = type.element();
        }
        switch (type.kind()) {
            case VOID -> {
                return null;
            }
            case STRUCT, TUPLE -> {
                return build(type, null, directFields(type.fields(), slots, at, depth));
            }
            case ENUM -> {
                Type.Case c = variant(type, slots[at]);
                return build(type, c, directFields(c.payload(), slots, at, depth));
            }
            case LIST -> {
                int d = deeper(depth);
                long bits = slots[at];
                Type e = type.element();
                int n = readers.listLength(bits);
                long[] cell = new long[Math.max(1, e.slots())];
                List<Object> out = new ArrayList<>(n);
                boolean flat = flat(e);
                for (int i = 0; i < n; ++i) {
                    readers.listGet(type, bits, i, cell);
                    out.add(flat ? Slots.box(e, cell[0], readers) : direct(e, cell, 0, d));
                }
                return out;
            }
            case MAP -> {
                int d = deeper(depth);
                long bits = slots[at];
                long[] key = new long[Math.max(1, type.key().slots())];
                long[] value = new long[Math.max(1, type.element().slots())];
                long n = readers.mapLength(bits);
                Map<Object, Object> out = new LinkedHashMap<>();
                for (long i = 0; i < n; ++i) {
                    readers.mapEntry(type, bits, i, key, value);
                    Object k = direct(type.key(), key, 0, d);
                    out.put(k, direct(type.element(), value, 0, d));
                }
                return out;
            }
            default -> {
                return Slots.box(type, slots[at], readers);
            }
        }
    }

    private Object[] directFields(List<Type.Field> fields, long[] slots, int at, int depth) {
        Object[] values = new Object[fields.size()];
        for (int i = 0; i < values.length; ++i) {
            Type.Field f = fields.get(i);
            values[i] = f.boxed() ? direct(f.type(), unbox(f, slots[at + f.offset()]), 0, deeper(depth))
                    : direct(f.type(), slots, at + f.offset(), depth);
        }
        return values;
    }

    private static int deeper(int depth) {
        if (depth + 1 >= Walk.DIRECT) {
            throw Walk.TooDeep.INSTANCE;
        }
        return depth + 1;
    }

    private Object step(Type type, long[] slots, int at) {
        while (type.kind() == Kind.NULLABLE) {
            if (slots[at] == 0) {
                return null;
            }
            at = type.isPointerNullable() ? at : at + 1;
            type = type.element();
        }
        return switch (type.kind()) {
            case VOID -> null;
            case STRUCT, TUPLE -> new Fields(type, null, type.fields(), slots, at);
            case ENUM -> {
                Type.Case c = variant(type, slots[at]);
                yield new Fields(type, c, c.payload(), slots, at);
            }
            case LIST -> new ListNode(type, slots[at]);
            case MAP -> new MapNode(type, slots[at]);
            default -> Slots.box(type, slots[at], readers);
        };
    }

    private static Type.Case variant(Type type, long index) {
        if (index < 0 || index >= type.cases().size()) {
            throw new QuartzException("enum value with an unknown variant " + index);
        }
        return type.cases().get((int) index);
    }

    /** The slots of the one value a recursive payload's box holds. */
    private long[] unbox(Type.Field f, long bits) {
        int n = readers.listLength(bits);
        if (n != 1) {
            throw new QuartzException("a recursive payload holds " + n + " values instead of one");
        }
        long[] one = new long[Math.max(1, f.type().slots())];
        readers.listGet(f.box(), bits, 0, one);
        return one;
    }

    private static Object build(Type type, Type.Case variant, Object[] values) {
        if (variant == null) {
            return type.kind() == Kind.STRUCT ? new Struct(type, values)
                    : Collections.unmodifiableList(Arrays.asList(values));
        }
        if (type.isResult()) {
            return variant.index() == 0 ? Result.ok(values[0]) : Result.err(values[0]);
        }
        return new Variant(variant.name(), Arrays.asList(values));
    }

    private static boolean flat(Type element) {
        Kind k = element.kind();
        return element.slots() == 1 && !Slots.isComposite(k) && k != Kind.LIST && k != Kind.MAP;
    }

    /** The fields of a struct or tuple, or the payload of a variant, all in the same slots. */
    private final class Fields extends Walk.Node {
        private final Type type;
        private final Type.Case variant;
        private final List<Type.Field> fields;
        private final long[] slots;
        private final int at;
        private final Object[] values;

        Fields(Type type, Type.Case variant, List<Type.Field> fields, long[] slots, int at) {
            this.type = type;
            this.variant = variant;
            this.fields = fields;
            this.slots = slots;
            this.at = at;
            this.values = new Object[fields.size()];
        }

        @Override
        int size() {
            return values.length;
        }

        @Override
        Object part(int i) {
            Type.Field f = fields.get(i);
            if (!f.boxed()) {
                return step(f.type(), slots, at + f.offset());
            }
            return step(f.type(), unbox(f, slots[at + f.offset()]), 0);
        }

        @Override
        void accept(int i, Object value) {
            values[i] = value;
        }

        @Override
        Object finish() {
            return build(type, variant, values);
        }
    }

    private final class ListNode extends Walk.Node {
        private final Type type;
        private final Type element;
        private final long bits;
        private final long[] cell;
        private final List<Object> out;
        private final int size;

        ListNode(Type type, long bits) {
            this.type = type;
            this.element = type.element();
            this.bits = bits;
            int n = readers.listLength(bits);
            this.cell = new long[Math.max(1, element.slots())];
            this.out = new ArrayList<>(n);
            if (flat(element)) {
                for (int i = 0; i < n; ++i) {
                    readers.listGet(type, bits, i, cell);
                    out.add(Slots.box(element, cell[0], readers));
                }
                n = 0;
            }
            this.size = n;
        }

        @Override
        int size() {
            return size;
        }

        @Override
        Object part(int i) {
            readers.listGet(type, bits, i, cell);
            return step(element, cell, 0);
        }

        @Override
        void accept(int i, Object value) {
            out.add(value);
        }

        @Override
        Object finish() {
            return out;
        }
    }

    /** Entry {@code i} is parts {@code 2i} (its key) and {@code 2i + 1} (its value). */
    private final class MapNode extends Walk.Node {
        private final Type type;
        private final long bits;
        private final long[] key;
        private final long[] value;
        private final Map<Object, Object> out = new LinkedHashMap<>();
        private final int size;
        private Object pendingKey;

        MapNode(Type type, long bits) {
            this.type = type;
            this.bits = bits;
            this.key = new long[Math.max(1, type.key().slots())];
            this.value = new long[Math.max(1, type.element().slots())];
            this.size = Math.toIntExact(2 * readers.mapLength(bits));
        }

        @Override
        int size() {
            return size;
        }

        @Override
        Object part(int i) {
            if (i % 2 == 1) {
                return step(type.element(), value, 0);
            }
            readers.mapEntry(type, bits, i / 2, key, value);
            return step(type.key(), key, 0);
        }

        @Override
        void accept(int i, Object v) {
            if (i % 2 == 0) {
                pendingKey = v;
            } else {
                out.put(pendingKey, v);
            }
        }

        @Override
        Object finish() {
            return out;
        }
    }
}
