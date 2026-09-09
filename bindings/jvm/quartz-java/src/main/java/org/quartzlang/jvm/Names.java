package org.quartzlang.jvm;

import java.util.Locale;

/** Matching of Quartz names to Java ones: exactly, or else ignoring case and underscores. */
final class Names {
    private Names() {}

    /** {@code IN_PROGRESS}, {@code InProgress} and {@code in_progress} all become {@code inprogress}. */
    static String normalize(String name) {
        return name.replace("_", "").toLowerCase(Locale.ROOT);
    }
}
