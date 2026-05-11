package org.quartzlang.jvm;

/**
 * A parameter or result type as far as the C interface describes it.
 *
 * @param kind what the type is
 * @param slots how many value slots it takes (a struct takes one per field)
 */
public record Type(Kind kind, int slots) {
    @Override
    public String toString() {
        return slots == 1 ? kind.name().toLowerCase(java.util.Locale.ROOT)
                : kind.name().toLowerCase(java.util.Locale.ROOT) + "[" + slots + "]";
    }
}
