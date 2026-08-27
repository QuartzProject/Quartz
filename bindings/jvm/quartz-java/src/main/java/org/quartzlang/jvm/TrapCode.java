package org.quartzlang.jvm;

/** Why a program trapped, in the order of {@code qz_trap}. */
public enum TrapCode {
    NONE,
    UNREACHABLE,
    DIVIDE_BY_ZERO,
    INDEX_OUT_OF_BOUNDS,
    NULL_UNWRAP,
    OUT_OF_FUEL,
    OUT_OF_MEMORY,
    STACK_OVERFLOW,
    INVALID_HANDLE,
    PANIC,
    HOST_ERROR;

    private static final TrapCode[] VALUES = values();

    static TrapCode of(int code) {
        return code >= 0 && code < VALUES.length ? VALUES[code] : PANIC;
    }
}
