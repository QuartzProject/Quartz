package org.quartzlang.jvm;

import java.util.Locale;

/**
 * A parameter or result type as the C interface describes it.
 *
 * @param kind what the type is
 * @param slots how many value slots it takes (a struct takes one per field)
 * @param id the program's type id, 0 when unknown
 * @param element the element of a list or nullable, or null
 */
public record Type(Kind kind, int slots, int id, Type element) {
    public Type(Kind kind, int slots) {
        this(kind, slots, 0, null);
    }

    /** A one-slot nullable str, Bytes or list, which is null or a pointer. */
    public boolean isNullableReference() {
        return kind == Kind.NULLABLE && slots == 1 && element != null && element.kind.isReference();
    }

    @Override
    public boolean equals(Object o) {
        return o instanceof Type t && t.kind == kind && t.slots == slots
                && (t.element == null ? element == null : t.element.equals(element));
    }

    @Override
    public int hashCode() {
        return kind.hashCode() * 31 + slots;
    }

    @Override
    public String toString() {
        String name = kind.name().toLowerCase(Locale.ROOT);
        String inner = element == null ? "" : "<" + element + ">";
        return slots == 1 ? name + inner : name + inner + "[" + slots + "]";
    }
}
