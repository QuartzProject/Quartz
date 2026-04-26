package org.quartzlang.jvm;

import java.util.Objects;

/** One source file; a name ending in {@code .qzi} is a host interface. */
public record Source(String name, String text) {
    public Source {
        Objects.requireNonNull(name, "name");
        Objects.requireNonNull(text, "text");
    }

    public static Source of(String name, String text) {
        return new Source(name, text);
    }
}
