package org.quartzlang.jvm;

/** What the compiler does with a warning rule, in the order of {@code qz_level}. */
public enum WarningLevel {
    /** Not reported. */
    IGNORE,
    /** Reported; the program still compiles. */
    WARNING,
    /** Reported as an error; the compilation fails. */
    ERROR
}
