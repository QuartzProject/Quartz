package org.quartzlang.jvm;

import java.util.Objects;

/**
 * Conversions between the values the bindings give and take ({@link Value}, {@link Struct}, {@link Variant},
 * {@link Result}, lists, maps and boxed scalars) and host classes: records, Kotlin data classes, enums and sealed
 * types matched by field and variant name, exactly or else ignoring case and underscores.
 */
public final class Values {
    private Values() {}

    /** {@code value} (a {@link Value} or what {@link Value#get()} gives) converted to {@code target}. */
    public static <T> T to(Object value, Class<T> target) {
        Object o = Mapper.convert(unwrap(value), target);
        return o == null ? null : cast(target, o);
    }

    /** {@code value} converted to a generic type: {@code Values.to(v, new TypeRef<List<Point>>() {})}. */
    @SuppressWarnings("unchecked")
    public static <T> T to(Object value, TypeRef<T> target) {
        return (T) Mapper.convert(unwrap(value), target.type());
    }

    /** {@code value} converted to a generic type given as a {@link java.lang.reflect.Type}. */
    public static Object to(Object value, java.lang.reflect.Type target) {
        return Mapper.convert(unwrap(value), target);
    }

    /**
     * {@code value} converted to {@code target} following its Quartz type, so every handle inside becomes the object
     * {@code handles} gives for it (a {@code long} or {@code Long} target keeps the id).
     *
     * @param quartz the value's type; null takes the type of a {@link Value}
     */
    public static Object to(Object value, Type quartz, java.lang.reflect.Type target, HandleMapper handles) {
        Type type = quartz == null && value instanceof Value v ? v.type() : quartz;
        return TypedValues.to(unwrap(value), type, target, handles);
    }

    /**
     * {@code value} (a record, data class, enum constant, sealed subclass, Map, List, array, Optional or scalar) as
     * what {@link Value#get()} would give for {@code quartz}: a {@link Struct} of that type, a {@link Variant} or
     * {@link Result}, lists and maps of converted elements, Long for every integer and handle, Integer for a char.
     * The result can be passed to a call or returned from a host function.
     *
     * @throws IllegalArgumentException when {@code value} does not fit {@code quartz}
     */
    public static Object from(Object value, Type quartz) {
        return from(value, quartz, null);
    }

    /** Like {@link #from(Object, Type)}, turning host objects where a handle goes into ids with {@code handles}. */
    public static Object from(Object value, Type quartz, HandleMapper handles) {
        return TypedValues.from(value, Objects.requireNonNull(quartz, "quartz"), handles);
    }

    private static Object unwrap(Object value) {
        return value instanceof Value v ? v.get() : value;
    }

    @SuppressWarnings("unchecked")
    private static <T> T cast(Class<T> target, Object o) {
        return (T) Mapper.box(target).cast(o);
    }
}
