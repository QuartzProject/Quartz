package org.quartzlang.jvm;

import java.nio.charset.StandardCharsets;
import java.util.List;

/** One result slot of a known type; a str or list result is readable until the next call into its sandbox. */
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

    /** Every raw slot, as many as {@code type().slots()}: how a struct or tuple result comes back for now. */
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

    /** The value boxed by its type: Long, Double, Float, Boolean, Integer (char), String, byte[], List, long[] for a
     wider value, or null for void and an empty nullable. */
    public Object get() {
        if (wide != null) {
            return wide.clone();
        }
        return Slots.box(type, bits, owner.readers(generation));
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

    private IllegalStateException wrong(String what) {
        return new IllegalStateException("the value is " + type + ", " + what + " does not apply");
    }

    @Override
    public String toString() {
        return "Value[" + type + " 0x" + Long.toHexString(bits) + "]";
    }
}
