package org.quartzlang.jvm;

import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/** Converts between Java values and the slots of a Quartz value by the type the program declares. */
final class Slots {
    /** Makes str, list and map slots in whatever arena is current (outside a call, or inside a host function). */
    interface Makers {
        long string(byte[] utf8) throws Throwable;

        long bytes(byte[] data) throws Throwable;

        long list(Type type, List<?> elements) throws Throwable;

        long map(Type type, Map<?, ?> entries) throws Throwable;
    }

    /** Reads what str, list and map slots point to. */
    interface Readers {
        byte[] string(long bits);

        List<Object> list(Type type, long bits);

        Map<Object, Object> map(Type type, long bits);
    }

    private Slots() {}

    /** One slot of a one-slot type; throws IllegalArgumentException when {@code value} does not fit. */
    static long encode(Type type, Object value, Sandbox owner, Makers makers) throws Throwable {
        if (type.kind() == Kind.I64 && value instanceof Long l) {
            return l;
        }
        if (isComposite(type.kind())) {
            long[] out = new long[Math.max(1, type.slots())];
            encode(type, value, out, 0, owner, makers);
            return out[0];
        }
        return scalar(type, value, owner, makers);
    }

    /** Writes {@code value} as {@code type} into {@code out[at..at + type.slots())}. */
    static void encode(Type type, Object value, long[] out, int at, Sandbox owner, Makers makers) throws Throwable {
        if (value instanceof Value v && isComposite(type.kind())) {
            passBackWide(type, v, out, at, owner);
            return;
        }
        switch (type.kind()) {
            case VOID -> {}
            case STRUCT -> fill(type.fields(), structValues(type, value), out, at, owner, makers);
            case TUPLE -> fill(type.fields(), tupleValues(type, value), out, at, owner, makers);
            case ENUM -> {
                Variant v = variantOf(type, value);
                Type.Case c = type.findCase(v.name());
                if (c == null) {
                    throw new IllegalArgumentException("no variant " + v.name() + " in " + type + "; it has "
                            + type.cases().stream().map(Type.Case::name).toList());
                }
                if (c.payload().size() != v.values().size()) {
                    throw new IllegalArgumentException("variant " + c.name() + " takes " + c.payload().size()
                            + " values, got " + v.values().size());
                }
                Arrays.fill(out, at, at + type.slots(), 0L);
                out[at] = c.index();
                fill(c.payload(), v.values().toArray(), out, at, owner, makers);
            }
            case NULLABLE -> {
                Arrays.fill(out, at, at + type.slots(), 0L);
                if (value == null) {
                    return;
                }
                if (type.isPointerNullable()) {
                    encode(type.element(), value, out, at, owner, makers);
                } else {
                    out[at] = 1;
                    encode(type.element(), value, out, at + 1, owner, makers);
                }
            }
            default -> out[at] = scalar(type, value, owner, makers);
        }
    }

    /** Reads a value of {@code type} from {@code slots[at..]}: see {@link Value#get()} for what each type gives. */
    static Object decode(Type type, long[] slots, int at, Readers readers) {
        return switch (type.kind()) {
            case VOID -> null;
            case STRUCT -> new Struct(type, read(type.fields(), slots, at, readers));
            case TUPLE -> Collections.unmodifiableList(Arrays.asList(read(type.fields(), slots, at, readers)));
            case ENUM -> {
                long index = slots[at];
                if (index < 0 || index >= type.cases().size()) {
                    throw new QuartzException("enum value with an unknown variant " + index);
                }
                Type.Case c = type.cases().get((int) index);
                Object[] values = read(c.payload(), slots, at, readers);
                if (type.isResult()) {
                    yield c.index() == 0 ? Result.ok(values[0]) : Result.err(values[0]);
                }
                yield new Variant(c.name(), Arrays.asList(values));
            }
            case NULLABLE -> {
                if (slots[at] == 0) {
                    yield null;
                }
                yield decode(type.element(), slots, type.isPointerNullable() ? at : at + 1, readers);
            }
            default -> box(type, slots[at], readers);
        };
    }

    /** A one-slot value as the Java value it maps to. */
    static Object box(Type type, long bits, Readers readers) {
        Kind kind = type.kind();
        if (kind.isInteger() || kind == Kind.HANDLE) {
            return widen(kind, bits);
        }
        return switch (kind) {
            case VOID -> null;
            case BOOL -> bits != 0;
            case F64 -> Double.longBitsToDouble(bits);
            case F32 -> Float.intBitsToFloat((int) bits);
            case CHAR -> (int) bits;
            case STR -> new String(readers.string(bits), StandardCharsets.UTF_8);
            case BYTES -> readers.string(bits);
            case LIST -> readers.list(type, bits);
            case MAP -> readers.map(type, bits);
            case STRUCT, TUPLE, ENUM, NULLABLE -> decode(type, new long[] {bits}, 0, readers);
            default -> throw new UnsupportedOperationException("a " + kind + " value cannot be read through the "
                    + "C interface");
        };
    }

    /** An integer slot as a long: signed kinds sign-extended from their width, unsigned ones zero-extended. */
    static long widen(Kind kind, long bits) {
        int width = kind.bits();
        if (width == 0 || width == 64) {
            return bits;
        }
        int shift = 64 - width;
        return kind.isSigned() ? bits << shift >> shift : bits << shift >>> shift;
    }

    /** True when the type, or anything inside it, points into the sandbox's memory. */
    static boolean holdsReferences(Type type) {
        return switch (type.kind()) {
            case STR, BYTES, LIST, MAP, FUNCTION -> true;
            case NULLABLE -> type.element() == null || holdsReferences(type.element());
            case STRUCT, TUPLE -> type.fields().stream().anyMatch(f -> holdsReferences(f.type()));
            case ENUM -> type.cases().stream().anyMatch(c -> c.payload().stream()
                    .anyMatch(f -> holdsReferences(f.type())));
            default -> false;
        };
    }

    static boolean isComposite(Kind kind) {
        return kind == Kind.STRUCT || kind == Kind.TUPLE || kind == Kind.ENUM || kind == Kind.NULLABLE;
    }

    static IllegalArgumentException mismatch(Type type, Object value) {
        return new IllegalArgumentException("a " + value.getClass().getSimpleName() + " cannot be passed as " + type);
    }

    private static void fill(List<Type.Field> fields, Object[] values, long[] out, int at, Sandbox owner,
            Makers makers) throws Throwable {
        for (int i = 0; i < values.length; ++i) {
            Type.Field f = fields.get(i);
            encode(f.type(), values[i], out, at + f.offset(), owner, makers);
        }
    }

    private static Object[] read(List<Type.Field> fields, long[] slots, int at, Readers readers) {
        Object[] values = new Object[fields.size()];
        for (int i = 0; i < values.length; ++i) {
            Type.Field f = fields.get(i);
            values[i] = decode(f.type(), slots, at + f.offset(), readers);
        }
        return values;
    }

    private static long scalar(Type type, Object value, Sandbox owner, Makers makers) throws Throwable {
        Kind kind = type.kind();
        if (value == null) {
            throw new IllegalArgumentException("null cannot be passed as " + type);
        }
        if (value instanceof Value v) {
            return passBack(type, v, owner);
        }
        if (kind.isInteger()) {
            return integer(kind, value);
        }
        return switch (kind) {
            case BOOL -> {
                if (!(value instanceof Boolean b)) {
                    throw mismatch(type, value);
                }
                yield b ? 1 : 0;
            }
            case F64 -> Double.doubleToRawLongBits(floating(type, value));
            case F32 -> Float.floatToRawIntBits((float) floating(type, value)) & 0xFFFF_FFFFL;
            case CHAR -> codePoint(type, value);
            case STR -> {
                if (!(value instanceof String s)) {
                    throw mismatch(type, value);
                }
                yield makers.string(s.getBytes(StandardCharsets.UTF_8));
            }
            case BYTES -> {
                if (!(value instanceof byte[] b)) {
                    throw mismatch(type, value);
                }
                yield makers.bytes(b);
            }
            case HANDLE -> {
                if (!(value instanceof Long || value instanceof Integer)) {
                    throw mismatch(type, value);
                }
                yield ((Number) value).longValue();
            }
            case LIST -> {
                if (value instanceof Object[] array) {
                    yield makers.list(type, Arrays.asList(array));
                }
                if (!(value instanceof List<?> list)) {
                    throw mismatch(type, value);
                }
                yield makers.list(type, list);
            }
            case MAP -> {
                if (!(value instanceof Map<?, ?> map)) {
                    throw mismatch(type, value);
                }
                yield makers.map(type, map);
            }
            default -> throw new IllegalArgumentException("a " + type + " value cannot cross the C interface");
        };
    }

    private static Object[] structValues(Type type, Object value) {
        if (value == null) {
            throw new IllegalArgumentException("null cannot be passed as " + type);
        }
        List<Type.Field> fields = type.fields();
        Object[] out = new Object[fields.size()];
        if (value instanceof Struct s) {
            if (!Names.sameType(type.name(), s.type().name())) {
                throw new IllegalArgumentException("a struct " + s.type().name() + " cannot be passed as "
                        + type.name());
            }
            for (int i = 0; i < out.length; ++i) {
                int index = s.indexOf(fields.get(i).name());
                if (index < 0) {
                    throw new IllegalArgumentException("the struct passed lacks field " + fields.get(i).name());
                }
                out[i] = s.values()[index];
            }
            return out;
        }
        if (value instanceof Map<?, ?> map) {
            for (int i = 0; i < out.length; ++i) {
                String name = fields.get(i).name();
                if (!map.containsKey(name) && fields.get(i).type().kind() != Kind.NULLABLE) {
                    throw new IllegalArgumentException("the map passed as a struct lacks field " + name);
                }
                out[i] = map.get(name);
            }
            for (Object k : map.keySet()) {
                if (!(k instanceof String name) || fields.stream().noneMatch(f -> f.name().equals(name))) {
                    throw new IllegalArgumentException("the struct has no field " + k + "; it has "
                            + fields.stream().map(Type.Field::name).toList());
                }
            }
            return out;
        }
        return Mapper.structValues(value, type);
    }

    private static Object[] tupleValues(Type type, Object value) {
        int n = type.fields().size();
        Object[] out;
        if (value instanceof List<?> list) {
            out = list.toArray();
        } else if (value instanceof Struct s) {
            out = s.values();
        } else if (value instanceof Object[] array) {
            out = array;
        } else if (value == null) {
            throw new IllegalArgumentException("null cannot be passed as " + type);
        } else {
            out = Mapper.components(value);
        }
        if (out.length != n) {
            throw new IllegalArgumentException("a tuple of " + n + " cannot take " + out.length + " values");
        }
        return out;
    }

    private static Variant variantOf(Type type, Object value) {
        if (value instanceof Variant v) {
            return v;
        }
        if (value instanceof Result<?, ?> r) {
            return switch (r) {
                case Result.Ok<?, ?> ok -> new Variant("Ok", Collections.singletonList(ok.value()));
                case Result.Err<?, ?> err -> new Variant("Err", Collections.singletonList(err.error()));
            };
        }
        if (type.isResult()) {
            return new Variant("Ok", Collections.singletonList(value));
        }
        if (value == null) {
            throw new IllegalArgumentException("null cannot be passed as " + type);
        }
        if (value instanceof String name) {
            return new Variant(name, List.of());
        }
        if (value instanceof Enum<?> e) {
            return new Variant(e.name(), List.of());
        }
        return Mapper.variant(value);
    }

    private static void passBackWide(Type type, Value v, long[] out, int at, Sandbox owner) {
        if (!v.type().equals(type)) {
            throw new IllegalArgumentException("a " + v.type() + " result cannot be passed as " + type);
        }
        if (holdsReferences(type) && (v.owner() != owner || !owner.isCurrent(v.generation()))) {
            throw new IllegalArgumentException("a result holding strings, lists or maps is only valid in its "
                    + "sandbox until the next call");
        }
        long[] slots = v.slots();
        System.arraycopy(slots, 0, out, at, Math.min(slots.length, type.slots()));
    }

    private static long passBack(Type type, Value v, Sandbox owner) {
        Kind kind = type.kind();
        boolean same = v.kind() == kind || (v.kind().isInteger() && kind.isInteger())
                || (v.kind() == Kind.STR && kind == Kind.BYTES);
        if (!same || kind == Kind.FUNCTION || ((kind == Kind.LIST || kind == Kind.MAP) && !type.equals(v.type()))) {
            throw new IllegalArgumentException("a " + v.kind() + " result cannot be passed as " + type);
        }
        if (kind.isReference() && (v.owner() != owner || !owner.isCurrent(v.generation()))) {
            throw new IllegalArgumentException("a str, list or map result is only valid in its sandbox until the "
                    + "next call");
        }
        return kind.isInteger() ? integer(kind, Slots.widen(v.kind(), v.bits())) : v.bits();
    }

    private static long integer(Kind kind, Object value) {
        long n;
        if (value instanceof Long || value instanceof Integer || value instanceof Short || value instanceof Byte) {
            n = ((Number) value).longValue();
        } else if (value instanceof Character c) {
            n = c;
        } else {
            throw new IllegalArgumentException("a " + value.getClass().getSimpleName() + " cannot be passed as "
                    + kind.name().toLowerCase(Locale.ROOT));
        }
        return integer(kind, n);
    }

    private static long integer(Kind kind, long n) {
        int width = kind.bits();
        boolean fits = width == 64 || (kind.isSigned() ? n >= -(1L << (width - 1)) && n < 1L << (width - 1)
                : n >= 0 && n < 1L << width);
        if (!fits) {
            throw new IllegalArgumentException(n + " does not fit in " + kind.name().toLowerCase(Locale.ROOT));
        }
        return n;
    }

    private static double floating(Type type, Object value) {
        if (value instanceof Double || value instanceof Float || value instanceof Long || value instanceof Integer
                || value instanceof Short || value instanceof Byte) {
            return ((Number) value).doubleValue();
        }
        throw mismatch(type, value);
    }

    private static long codePoint(Type type, Object value) {
        int cp;
        if (value instanceof Character c) {
            cp = c;
        } else if (value instanceof Integer i) {
            cp = i;
        } else {
            throw mismatch(type, value);
        }
        if (!Character.isValidCodePoint(cp) || (cp >= 0xD800 && cp <= 0xDFFF)) {
            throw new IllegalArgumentException("U+" + Integer.toHexString(cp) + " is not a Unicode scalar value");
        }
        return cp;
    }
}
