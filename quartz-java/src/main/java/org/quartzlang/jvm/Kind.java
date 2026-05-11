package org.quartzlang.jvm;

/** The kind of a Quartz type as the C interface reports it, in the order of {@code qz_kind}. */
public enum Kind {
    VOID,
    BOOL,
    I8,
    I16,
    I32,
    I64,
    U8,
    U16,
    U32,
    U64,
    F32,
    F64,
    CHAR,
    STR,
    BYTES,
    LIST,
    MAP,
    STRUCT,
    ENUM,
    NULLABLE,
    TUPLE,
    HANDLE,
    FUNCTION,
    OTHER;

    private static final Kind[] VALUES = values();

    static Kind of(int code) {
        return code >= 0 && code < VALUES.length ? VALUES[code] : OTHER;
    }

    public boolean isInteger() {
        return ordinal() >= I8.ordinal() && ordinal() <= U64.ordinal();
    }

    public boolean isSigned() {
        return ordinal() >= I8.ordinal() && ordinal() <= I64.ordinal();
    }

    public boolean isFloat() {
        return this == F32 || this == F64;
    }

    /** True for kinds whose slot points into the sandbox, which a binding must never forge. */
    public boolean isReference() {
        return this == STR || this == BYTES || this == LIST || this == MAP || this == FUNCTION;
    }

    /** Bits of an integer kind, 0 for any other. */
    public int bits() {
        return switch (this) {
            case I8, U8 -> 8;
            case I16, U16 -> 16;
            case I32, U32 -> 32;
            case I64, U64 -> 64;
            default -> 0;
        };
    }
}
