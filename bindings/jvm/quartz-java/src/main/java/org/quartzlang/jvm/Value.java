package org.quartzlang.jvm;

import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Map;

/** A result of a known type; what points into the sandbox (str, lists, maps) is readable until its next call. */
public final class Value {
    private final long bits;
    private final Sandbox owner;
    private final long generation;
    private final Type type;
    private final long[] wide;

    Value(long bits, Sandbox owner, long generation, Type type) {
        this(bits, owner, generation, type, null);
    }

    Value(long bits, Sandbox owner, long generation, Type type, long[] wide) {
        this.bits = bits;
        this.owner = owner;
        this.generation = generation;
        this.type = type;
        this.wide = wide;
    }

    public Type type() {
        return type;
    }

    public Kind kind() {
        return type.kind();
    }

    /** The raw 64 bits of the slot (the first one of a wider value). */
    public long bits() {
        return bits;
    }

    /** Every raw slot, as many as {@code type().slots()}. */
    public long[] slots() {
        return wide == null ? new long[] {bits} : wide.clone();
    }

    /** An integer, handle, char, bool or enum tag, widened to 64 bits as its kind says. */
    public long asLong() {
        Kind k = kind();
        if (!(k.isInteger() || k == Kind.HANDLE || k == Kind.CHAR || k == Kind.BOOL || k == Kind.ENUM
                || k == Kind.OTHER)) {
            throw wrong("asLong");
        }
        return Slots.widen(k, bits);
    }

    public int asInt() {
        return (int) asLong();
    }

    public double asDouble() {
        return switch (kind()) {
            case F64, OTHER -> Double.longBitsToDouble(bits);
            case F32 -> Float.intBitsToFloat((int) bits);
            default -> throw wrong("asDouble");
        };
    }

    public float asFloat() {
        return switch (kind()) {
            case F32, OTHER -> Float.intBitsToFloat((int) bits);
            case F64 -> (float) Double.longBitsToDouble(bits);
            default -> throw wrong("asFloat");
        };
    }

    public boolean asBoolean() {
        if (kind() != Kind.BOOL && kind() != Kind.OTHER) {
            throw wrong("asBoolean");
        }
        return bits != 0;
    }

    /** A {@code char} result as a Unicode code point. */
    public int asCodePoint() {
        if (kind() != Kind.CHAR && kind() != Kind.OTHER) {
            throw wrong("asCodePoint");
        }
        return (int) bits;
    }

    /** A {@code str} (or {@code Bytes}) result, copied out. */
    public String asString() {
        return new String(asBytes(), StandardCharsets.UTF_8);
    }

    /** A {@code str} or {@code Bytes} result, copied out. */
    public byte[] asBytes() {
        if (kind() != Kind.STR && kind() != Kind.BYTES) {
            throw wrong("asBytes");
        }
        return owner.readString(bits, generation);
    }

    /**
     * The value boxed by its type: Long (integers and handles), Double, Float, Boolean, Integer (char), String,
     * byte[], List (lists and tuples), Map (in insertion order), {@link Struct}, {@link Variant}, {@link Result}, or
     * null for void and an empty nullable.
     */
    public Object get() {
        if (wide == null && type.slots() > 1) {
            throw new UnsupportedOperationException("only the first slot of this " + type + " is known");
        }
        Slots.Readers readers = owner.readers(generation);
        if (wide != null) {
            return Slots.decode(type, wide, 0, readers);
        }
        return Slots.box(type, bits, readers);
    }

    /** The value converted to {@code target}: a record or Kotlin data class (by field name), an enum, a sealed type
     whose subclasses are named like the variants, a List, Map, Optional, array or a boxed scalar. */
    public <T> T to(Class<T> target) {
        Object o = Mapper.convert(get(), target);
        return o == null ? null : box(target).cast(o);
    }

    /** The value converted to a generic type: {@code value.to(new TypeRef<List<Point>>() {})}. */
    @SuppressWarnings("unchecked")
    public <T> T to(TypeRef<T> target) {
        return (T) Mapper.convert(get(), target.type());
    }

    /** The value converted to a generic type given as a {@link java.lang.reflect.Type}. */
    public Object to(java.lang.reflect.Type target) {
        return Mapper.convert(get(), target);
    }

    /** The value converted to {@code target}, every handle inside mapped to its host object by {@code handles}. */
    public Object to(java.lang.reflect.Type target, HandleMapper handles) {
        return TypedValues.to(get(), type, target, handles);
    }

    public Struct asStruct() {
        if (kind() != Kind.STRUCT) {
            throw wrong("asStruct");
        }
        return (Struct) get();
    }

    /** An enum result; a {@code Result} reads as variant {@code Ok} or {@code Err} too. */
    public Variant asVariant() {
        if (kind() != Kind.ENUM) {
            throw wrong("asVariant");
        }
        return (Variant) Mapper.convert(get(), Variant.class);
    }

    public Result<Object, Object> asResult() {
        if (!type.isResult()) {
            throw wrong("asResult");
        }
        @SuppressWarnings("unchecked")
        Result<Object, Object> r = (Result<Object, Object>) get();
        return r;
    }

    /** A tuple result's elements. */
    @SuppressWarnings("unchecked")
    public List<Object> asTuple() {
        if (kind() != Kind.TUPLE) {
            throw wrong("asTuple");
        }
        return (List<Object>) get();
    }

    /** A map result in insertion order. */
    @SuppressWarnings("unchecked")
    public Map<Object, Object> asMap() {
        if (kind() != Kind.MAP) {
            throw wrong("asMap");
        }
        return (Map<Object, Object>) get();
    }

    /** True for an empty nullable and for void. */
    public boolean isNull() {
        return kind() == Kind.VOID || (kind() == Kind.NULLABLE && bits == 0);
    }

    /** Elements of a list result. */
    public int listLength() {
        requireList();
        return owner.listLength(bits, generation);
    }

    /** A list result with its elements boxed by the element type, nested lists included. */
    public List<Object> asList() {
        requireList();
        return owner.readers(generation).list(type, bits);
    }

    @SuppressWarnings("unchecked")
    public List<Long> asLongList() {
        requireElement(Kind.I64);
        return (List<Long>) (List<?>) asList();
    }

    @SuppressWarnings("unchecked")
    public List<Double> asDoubleList() {
        requireElement(Kind.F64);
        return (List<Double>) (List<?>) asList();
    }

    @SuppressWarnings("unchecked")
    public List<Boolean> asBooleanList() {
        requireElement(Kind.BOOL);
        return (List<Boolean>) (List<?>) asList();
    }

    @SuppressWarnings("unchecked")
    public List<String> asStringList() {
        requireElement(Kind.STR);
        return (List<String>) (List<?>) asList();
    }

    /** False once a later call may have freed what a str or list result points to. */
    public boolean isCurrent() {
        return owner == null || owner.isCurrent(generation);
    }

    Sandbox owner() {
        return owner;
    }

    long generation() {
        return generation;
    }

    private void requireElement(Kind kind) {
        requireList();
        Kind e = type.element().kind();
        boolean fits = kind == Kind.I64 ? e.isInteger() || e == Kind.HANDLE : e == kind;
        if (!fits) {
            throw wrong("a list of " + kind.name().toLowerCase(java.util.Locale.ROOT));
        }
    }

    private void requireList() {
        if (kind() != Kind.LIST) {
            throw wrong("a list read");
        }
    }

    @SuppressWarnings("unchecked")
    private static <T> Class<T> box(Class<T> c) {
        if (!c.isPrimitive()) {
            return c;
        }
        return (Class<T>) java.lang.invoke.MethodType.methodType(c).wrap().returnType();
    }

    private IllegalStateException wrong(String what) {
        return new IllegalStateException("the value is " + type + ", " + what + " does not apply");
    }

    @Override
    public String toString() {
        return "Value[" + type + " 0x" + Long.toHexString(bits) + "]";
    }
}
