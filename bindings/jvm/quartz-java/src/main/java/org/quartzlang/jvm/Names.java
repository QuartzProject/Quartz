package org.quartzlang.jvm;

import java.util.Locale;

/** Matching of Quartz names to Java ones: exactly, or else ignoring case and underscores. */
final class Names {
    private Names() {}

    /** {@code IN_PROGRESS}, {@code InProgress} and {@code in_progress} all become {@code inprogress}. */
    static String normalize(String name) {
        return name.replace("_", "").toLowerCase(Locale.ROOT);
    }

    /** True when two type names fit, generic arguments aside; an unknown (empty) name fits anything. */
    static boolean sameType(String a, String b) {
        String x = bare(a);
        String y = bare(b);
        return x.isEmpty() || y.isEmpty() || x.equals(y) || normalize(x).equals(normalize(y));
    }

    private static String bare(String name) {
        int generic = name.indexOf('<');
        return generic < 0 ? name : name.substring(0, generic);
    }
}
