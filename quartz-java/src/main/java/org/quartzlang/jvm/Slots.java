package org.quartzlang.jvm;

import java.nio.charset.StandardCharsets;

/** Converts between Java values and one qz_slot by the kind the program declares. */
final class Slots {
    /** Makes a str slot in whatever arena is current (outside a call, or inside a host function). */
    @FunctionalInterface
    interface Strings {
        long make(byte[] utf8) throws Throwable;
    }

    /** Reads the bytes a str slot points to. */
    @FunctionalInterface
    interface Reader {
        byte[] read(long bits);
    }

    private Slots() {}

    /** A slot the program reads as {@code type}; throws IllegalArgumentException when {@code value} does not fit. */
    static long encode(Type type, Object value, Sandbox owner, Strings strings) throws Throwable {
        Kind kind = type.kind();
        if (kind == Kind.I64 && value instanceof Long l) {
            return l;
        }
        if (type.slots() != 1) {
            throw new IllegalArgumentException("a " + type + " value cannot cross the C interface yet");
        }
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
                yield strings.make(s.getBytes(StandardCharsets.UTF_8));
            }
            case BYTES -> {
                if (!(value instanceof byte[] b)) {
                    throw mismatch(type, value);
                }
                yield strings.make(b);
            }
            case HANDLE -> {
                if (!(value instanceof Long || value instanceof Integer)) {
                    throw mismatch(type, value);
                }
                yield ((Number) value).longValue();
            }
            default -> throw new IllegalArgumentException("a " + type + " value cannot cross the C interface yet");
        };
    }

    /** The slot as the Java value it maps to: Long, Float, Double, Boolean, Integer (char), String, byte[]. */
    static Object box(Kind kind, long bits, Reader reader) {
        if (kind.isInteger() || kind == Kind.HANDLE) {
            return widen(kind, bits);
        }
        return switch (kind) {
            case VOID -> null;
            case BOOL -> bits != 0;
            case F64 -> Double.longBitsToDouble(bits);
            case F32 -> Float.intBitsToFloat((int) bits);
            case CHAR -> (int) bits;
            case STR -> new String(reader.read(bits), StandardCharsets.UTF_8);
            case BYTES -> reader.read(bits);
            default -> throw new UnsupportedOperationException("a " + kind + " value cannot be read through the "
                    + "C interface yet");
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

    static IllegalArgumentException mismatch(Type type, Object value) {
        return new IllegalArgumentException("a " + value.getClass().getSimpleName() + " cannot be passed as " + type);
    }

    private static long passBack(Type type, Value v, Sandbox owner) {
        Kind kind = type.kind();
        boolean same = v.kind() == kind || (v.kind().isInteger() && kind.isInteger())
                || (v.kind() == Kind.STR && kind == Kind.BYTES);
        if (!same || kind == Kind.LIST || kind == Kind.MAP || kind == Kind.FUNCTION) {
            throw new IllegalArgumentException("a " + v.kind() + " result cannot be passed as " + type);
        }
        if (kind.isReference() && (v.owner() != owner || !owner.isCurrent(v.generation()))) {
            throw new IllegalArgumentException("a str result only goes back to its own sandbox before its next call");
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
                    + kind.name().toLowerCase(java.util.Locale.ROOT));
        }
        return integer(kind, n);
    }

    private static long integer(Kind kind, long n) {
        int width = kind.bits();
        boolean fits = width == 64 || (kind.isSigned() ? n >= -(1L << (width - 1)) && n < 1L << (width - 1)
                : n >= 0 && n < 1L << width);
        if (!fits) {
            throw new IllegalArgumentException(n + " does not fit in " + kind.name().toLowerCase(java.util.Locale.ROOT));
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
