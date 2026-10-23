package org.quartzlang.jvm;

/**
 * How {@link Values} turns {@code @handle} values into host objects and back, wherever they sit in a value: alone, in
 * lists, maps, nullables, struct fields, tuples and enum payloads.
 */
public interface HandleMapper {
    /** The host object behind handle {@code id} of {@code type}; throw when the id is unknown. */
    Object object(long id, Type type);

    /** The id {@code object} travels as, for a handle of {@code type}. */
    long id(Object object, Type type);
}
