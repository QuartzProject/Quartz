package org.quartzlang.jvm;

import java.lang.invoke.MethodHandle;
import java.lang.reflect.Array;
import java.lang.reflect.GenericArrayType;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.TypeVariable;
import java.lang.reflect.WildcardType;
import java.util.ArrayList;
import java.util.Collection;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;

/** Typed views of Quartz values: records, Kotlin data classes, enums and sealed types, both ways. */
final class Mapper {
    private Mapper() {}

    /** The values of a struct's fields, read from {@code value} by field name. */
    static Object[] structValues(Object value, Type struct) {
        MethodHandle[] plan = Shape.of(value.getClass()).writePlan(struct);
        Object[] out = new Object[plan.length];
        try {
            for (int i = 0; i < plan.length; ++i) {
                out[i] = (Object) plan[i].invokeExact(value);
            }
        } catch (Throwable t) {
            throw new IllegalArgumentException("cannot read " + value.getClass().getSimpleName() + ": " + t, t);
        }
        return out;
    }

    /** The components of a record or data class in order, for a tuple or a variant's values. */
    static Object[] components(Object value) {
        Shape s = Shape.of(value.getClass());
        if (s.form == Shape.Form.SINGLETON) {
            return new Object[0];
        }
        if (s.form != Shape.Form.PRODUCT) {
            throw new IllegalArgumentException("a " + value.getClass().getSimpleName() + " cannot be passed as a "
                    + "tuple or variant: use a List, a record or a Kotlin data class"
                    + (s.problem == null ? "" : " (" + s.problem + ")"));
        }
        Object[] out = new Object[s.getters.length];
        try {
            for (int i = 0; i < out.length; ++i) {
                out[i] = (Object) s.getters[i].invokeExact(value);
            }
        } catch (Throwable t) {
            throw new IllegalArgumentException("cannot read " + value.getClass().getSimpleName() + ": " + t, t);
        }
        return out;
    }

    /** A record, data class or object of a sealed type as the variant named like its class. */
    static Variant variant(Object value) {
        Shape s = Shape.of(value.getClass());
        if (s.form != Shape.Form.PRODUCT && s.form != Shape.Form.SINGLETON) {
            throw new IllegalArgumentException("a " + value.getClass().getSimpleName() + " cannot be passed as an "
                    + "enum: use a Variant, an enum constant, or a record or data class named like the variant");
        }
        return new Variant(value.getClass().getSimpleName(), List.of(components(value)));
    }

    /** {@code value}, as {@link Value#get()} gives it, converted to {@code target}. */
    static Object convert(Object value, java.lang.reflect.Type target) {
        if (target instanceof Class<?> c) {
            return toClass(value, c);
        }
        if (target instanceof ParameterizedType p) {
            return toParameterized(value, p);
        }
        if (target instanceof GenericArrayType g) {
            return toArray(value, g.getGenericComponentType(), raw(g.getGenericComponentType()));
        }
        if (target instanceof WildcardType w) {
            return convert(value, w.getUpperBounds()[0]);
        }
        if (target instanceof TypeVariable<?> v) {
            return convert(value, v.getBounds()[0]);
        }
        throw new IllegalArgumentException("cannot read a Quartz value as " + target);
    }

    private static Object toParameterized(Object value, ParameterizedType p) {
        Class<?> raw = raw(p);
        java.lang.reflect.Type[] args = p.getActualTypeArguments();
        if (value == null) {
            return raw == Optional.class ? Optional.empty() : null;
        }
        if (raw == Optional.class) {
            return Optional.of(convert(value, args[0]));
        }
        if (raw.isAssignableFrom(List.class) && value instanceof List<?> list) {
            List<Object> out = new ArrayList<>(list.size());
            for (Object x : list) {
                out.add(convert(x, args[0]));
            }
            return Collections.unmodifiableList(out);
        }
        if (raw.isAssignableFrom(LinkedHashMap.class) && value instanceof Map<?, ?> map) {
            Map<Object, Object> out = new LinkedHashMap<>();
            for (Map.Entry<?, ?> e : map.entrySet()) {
                out.put(convert(e.getKey(), args[0]), convert(e.getValue(), args[1]));
            }
            return Collections.unmodifiableMap(out);
        }
        if (raw == Result.class && args.length == 2) {
            Result<?, ?> r = (Result<?, ?>) toClass(value, Result.class);
            return switch (r) {
                case Result.Ok<?, ?> ok -> Result.ok(convert(ok.value(), args[0]));
                case Result.Err<?, ?> err -> Result.err(convert(err.error(), args[1]));
            };
        }
        return toClass(value, raw);
    }

    private static Object toClass(Object value, Class<?> c) {
        if (c == Object.class) {
            return value;
        }
        if (value == null) {
            if (c.isPrimitive()) {
                throw new IllegalArgumentException("null cannot be read as " + c);
            }
            return c == Optional.class ? Optional.empty() : null;
        }
        if (c.isPrimitive() || Number.class.isAssignableFrom(c) || c == Boolean.class || c == Character.class) {
            return scalar(value, c);
        }
        if (c.isInstance(value)) {
            return value;
        }
        if (c == Optional.class) {
            return Optional.of(value);
        }
        if (c == Variant.class && value instanceof Result<?, ?> r) {
            return r instanceof Result.Ok<?, ?> ok ? Variant.of("Ok", ok.value())
                    : Variant.of("Err", ((Result.Err<?, ?>) r).error());
        }
        if (c == Result.class && value instanceof Variant v && v.values().size() == 1) {
            if (v.name().equals("Ok") || v.name().equals("Err")) {
                return v.name().equals("Ok") ? Result.ok(v.get(0)) : Result.err(v.get(0));
            }
        }
        if (c.isEnum()) {
            return constant(value, c);
        }
        if (c.isArray() && value instanceof List<?>) {
            return toArray(value, c.getComponentType(), c.getComponentType());
        }
        if (Collection.class.isAssignableFrom(c) && value instanceof List<?>) {
            return value;
        }
        Shape s = Shape.of(c);
        switch (s.form) {
            case PRODUCT -> {
                return product(value, s);
            }
            case SINGLETON -> {
                if (value instanceof Variant v && v.values().isEmpty()) {
                    return s.instance;
                }
            }
            case SEALED -> {
                String name = variantName(value);
                if (name != null) {
                    Class<?> sub = subclass(s, name);
                    if (sub == null) {
                        throw new IllegalArgumentException("no subclass of " + c.getSimpleName() + " is named like the "
                                + "variant " + name);
                    }
                    return toClass(value, sub);
                }
            }
            default -> {}
        }
        throw new IllegalArgumentException("a " + describe(value) + " cannot be read as " + c.getName()
                + (s.problem == null ? "" : " (" + s.problem + ")"));
    }

    private static Object product(Object value, Shape s) {
        Object[] args = new Object[s.names.length];
        if (value instanceof Struct st) {
            int[] plan = s.readPlan(st.type());
            for (int j = 0; j < args.length; ++j) {
                args[j] = convert(st.values()[plan[j]], s.types[j]);
            }
        } else {
            List<?> values = switch (value) {
                case List<?> list -> list;
                case Variant v -> v.values();
                case Result.Ok<?, ?> ok -> Collections.singletonList(ok.value());
                case Result.Err<?, ?> err -> Collections.singletonList(err.error());
                default -> throw new IllegalArgumentException("a " + describe(value) + " cannot be read as "
                        + s.type.getName());
            };
            if (values.size() != args.length) {
                throw new IllegalArgumentException(s.type.getSimpleName() + " has " + args.length + " components, "
                        + "the value has " + values.size());
            }
            for (int j = 0; j < args.length; ++j) {
                args[j] = convert(values.get(j), s.types[j]);
            }
        }
        try {
            return s.construct(args);
        } catch (Throwable t) {
            throw new IllegalArgumentException("cannot build " + s.type.getSimpleName() + ": " + t, t);
        }
    }

    private static String variantName(Object value) {
        return switch (value) {
            case Variant v -> v.name();
            case Result.Ok<?, ?> ok -> "Ok";
            case Result.Err<?, ?> err -> "Err";
            default -> null;
        };
    }

    static Class<?> subclass(Shape s, String name) {
        for (Class<?> sub : s.subclasses) {
            if (sub.getSimpleName().equals(name)) {
                return sub;
            }
        }
        String wanted = Names.normalize(name);
        for (Class<?> sub : s.subclasses) {
            if (Names.normalize(sub.getSimpleName()).equals(wanted)) {
                return sub;
            }
        }
        return null;
    }

    private static Object constant(Object value, Class<?> c) {
        String name = value instanceof String s ? s
                : value instanceof Variant v && v.values().isEmpty() ? v.name() : null;
        if (name == null) {
            throw new IllegalArgumentException("a " + describe(value) + " cannot be read as the enum " + c.getName());
        }
        Object[] constants = c.getEnumConstants();
        for (Object k : constants) {
            if (((Enum<?>) k).name().equals(name)) {
                return k;
            }
        }
        String wanted = Names.normalize(name);
        for (Object k : constants) {
            if (Names.normalize(((Enum<?>) k).name()).equals(wanted)) {
                return k;
            }
        }
        throw new IllegalArgumentException(c.getSimpleName() + " has no constant for the variant " + name);
    }

    private static Object toArray(Object value, java.lang.reflect.Type element, Class<?> raw) {
        if (value == null) {
            return null;
        }
        if (!(value instanceof List<?> list)) {
            throw new IllegalArgumentException("a " + describe(value) + " cannot be read as an array");
        }
        Object out = Array.newInstance(raw, list.size());
        for (int i = 0; i < list.size(); ++i) {
            Array.set(out, i, convert(list.get(i), element));
        }
        return out;
    }

    private static Object scalar(Object value, Class<?> c) {
        if ((c == long.class || c == Long.class) && integral(value)) {
            return ((Number) value).longValue();
        }
        if ((c == int.class || c == Integer.class) && integral(value)) {
            return Math.toIntExact(((Number) value).longValue());
        }
        if ((c == short.class || c == Short.class) && integral(value)) {
            long n = ((Number) value).longValue();
            if (n == (short) n) {
                return (short) n;
            }
        }
        if ((c == byte.class || c == Byte.class) && integral(value)) {
            long n = ((Number) value).longValue();
            if (n == (byte) n) {
                return (byte) n;
            }
        }
        if ((c == double.class || c == Double.class) && (value instanceof Double || value instanceof Float)) {
            return ((Number) value).doubleValue();
        }
        if ((c == float.class || c == Float.class) && (value instanceof Double || value instanceof Float)) {
            return ((Number) value).floatValue();
        }
        if ((c == boolean.class || c == Boolean.class) && value instanceof Boolean) {
            return value;
        }
        if ((c == char.class || c == Character.class) && value instanceof Integer cp && cp >= 0 && cp <= 0xFFFF) {
            return (char) cp.intValue();
        }
        if (c.isInstance(value)) {
            return value;
        }
        throw new IllegalArgumentException("a " + describe(value) + " cannot be read as " + c.getName());
    }

    private static boolean integral(Object value) {
        return value instanceof Long || value instanceof Integer || value instanceof Short || value instanceof Byte;
    }

    static Class<?> raw(java.lang.reflect.Type t) {
        return switch (t) {
            case Class<?> c -> c;
            case ParameterizedType p -> (Class<?>) p.getRawType();
            case GenericArrayType g -> raw(g.getGenericComponentType()).arrayType();
            default -> Object.class;
        };
    }

    static Class<?> box(Class<?> c) {
        return c.isPrimitive() ? java.lang.invoke.MethodType.methodType(c).wrap().returnType() : c;
    }

    static String describe(Object value) {
        return switch (value) {
            case Struct s -> "struct " + s.names();
            case Variant v -> "variant " + v.name();
            default -> value.getClass().getSimpleName() + " " + value;
        };
    }
}
