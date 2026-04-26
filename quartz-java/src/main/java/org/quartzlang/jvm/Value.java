package org.quartzlang.jvm;

import java.nio.charset.StandardCharsets;

/** One result slot, read as the type the function declares; a str result is readable until the next call. */
public final class Value {
    private final long bits;
    private final Sandbox owner;
    private final long generation;

    Value(long bits, Sandbox owner, long generation) {
        this.bits = bits;
        this.owner = owner;
        this.generation = generation;
    }

    /** The raw 64 bits of the slot. */
    public long bits() {
        return bits;
    }

    public long asLong() {
        return bits;
    }

    public int asInt() {
        return (int) bits;
    }

    public double asDouble() {
        return Double.longBitsToDouble(bits);
    }

    public float asFloat() {
        return Float.intBitsToFloat((int) bits);
    }

    public boolean asBoolean() {
        return bits != 0;
    }

    /** A {@code char} result as a Unicode code point. */
    public int asCodePoint() {
        return (int) bits;
    }

    /** Only for a {@code str} result: reading any other type as a string is undefined. */
    public String asString() {
        return new String(asBytes(), StandardCharsets.UTF_8);
    }

    /** Only for a {@code str} or {@code Bytes} result. */
    public byte[] asBytes() {
        if (owner == null) {
            throw new IllegalStateException("this value holds no string");
        }
        return owner.readString(bits, generation);
    }

    /** False once a later call may have freed what a str result points to. */
    public boolean isCurrent() {
        return owner == null || owner.isCurrent(generation);
    }

    Sandbox owner() {
        return owner;
    }

    long generation() {
        return generation;
    }

    @Override
    public String toString() {
        return "Value[0x" + Long.toHexString(bits) + "]";
    }
}
