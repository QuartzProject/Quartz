package org.quartzlang.jvm;

import java.nio.charset.StandardCharsets;
import java.util.List;

/** One result slot of a known type; a str or list result is readable until the next call into its sandbox. */
public final class Value {
    private final long bits;
    private final Sandbox owner;
    private final long generation;
    private final Type type;

    Value(long bits, Sandbox owner, long generation, Type type) {
        this.bits = bits;
        this.owner = owner;
        this.generation = generation;
        this.type = type;
    }

    public Type type() {
        return type;
    }

    public Kind kind() {
        return type.kind();
    }

    /** The raw 64 bits of the slot. */
    public long bits() {
        return bits;
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

    /** The value boxed by its kind: Long, Double, Float, Boolean, Integer (char), String, byte[], or null for void. */
    public Object get() {
        Kind k = kind();
        if (k == Kind.STR || k == Kind.BYTES) {
            byte[] b = asBytes();
            return k == Kind.STR ? new String(b, StandardCharsets.UTF_8) : b;
        }
        return Slots.box(k, bits, ignored -> {
            throw new IllegalStateException("unreachable");
        });
    }

    /** Elements of a list result. */
    public int listLength() {
        requireList();
        return owner.listLength(bits, generation);
    }

    /** A list result boxed as {@code element} says, which is unchecked: numbers read as STR are undefined. */
    public List<Object> asList(Kind element) {
        requireList();
        return owner.readList(bits, generation, element);
    }

    @SuppressWarnings("unchecked")
    public List<Long> asLongList() {
        return (List<Long>) (List<?>) asList(Kind.I64);
    }

    @SuppressWarnings("unchecked")
    public List<Double> asDoubleList() {
        return (List<Double>) (List<?>) asList(Kind.F64);
    }

    @SuppressWarnings("unchecked")
    public List<Boolean> asBooleanList() {
        return (List<Boolean>) (List<?>) asList(Kind.BOOL);
    }

    /** Only for a {@code List<str>}: see {@link #asList(Kind)}. */
    @SuppressWarnings("unchecked")
    public List<String> asStringList() {
        return (List<String>) (List<?>) asList(Kind.STR);
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
