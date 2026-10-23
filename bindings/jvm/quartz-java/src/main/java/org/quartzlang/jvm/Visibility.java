package org.quartzlang.jvm;

/** What a function's declaration wrote, in the order of {@code qz_visibility}. */
public enum Visibility {
    /** Nothing written. */
    DEFAULT,
    /** {@code private}: not meant for the host. */
    PRIVATE,
    INTERNAL,
    /** {@code pub}. */
    PUBLIC;

    static Visibility of(int code) {
        return code >= 0 && code < 4 ? values()[code] : DEFAULT;
    }
}
