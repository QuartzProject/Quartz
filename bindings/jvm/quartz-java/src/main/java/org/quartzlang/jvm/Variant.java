package org.quartzlang.jvm;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Objects;

/**
 * A value of a Quartz enum: which variant it is and the values it carries, in order.
 *
 * @param name the variant name, as the program spells it
 * @param values its payload; empty for a variant without one
 */
public record Variant(String name, List<Object> values) {
    public Variant {
        Objects.requireNonNull(name, "name");
        values = values == null ? List.of() : Collections.unmodifiableList(new ArrayList<>(values));
    }

    /** A variant built by name, as an argument or a host function's result. */
    public static Variant of(String name, Object... values) {
        return new Variant(name, Arrays.asList(values));
    }

    /** Payload value {@code index}. */
    public Object get(int index) {
        return values.get(index);
    }

    @Override
    public String toString() {
        return values.isEmpty() ? name : name + values.toString().replace('[', '(').replace(']', ')');
    }
}
