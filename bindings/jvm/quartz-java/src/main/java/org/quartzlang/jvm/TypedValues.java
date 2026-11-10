package org.quartzlang.jvm;

import java.lang.reflect.Array;
import java.lang.reflect.GenericArrayType;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.WildcardType;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;

/** Conversions that follow the Quartz type of a value, so handles map to host objects at any depth. */
final class TypedValues {
    private TypedValues() {}

    /** {@code value}, boxed as {@link Value#get()} gives it for {@code quartz}, converted to {@code target}. */
    static Object to(Object value, Type quartz, java.lang.reflect.Type target, HandleMapper handles) {
        return Walk.convert(depth -> toStep(value, quartz, target, handles, depth), handles == null);
    }

    /** {@code value} as the generic form of {@code quartz}: what {@link Value#get()} would give for it. */
    static Object from(Object value, Type quartz, HandleMapper handles) {
        return Walk.convert(depth -> fromStep(value, quartz, handles, depth), handles == null);
    }

    private static Object toStep(Object value, Type quartz, java.lang.reflect.Type target, HandleMapper handles,
            int depth) {
        int d = Walk.deeper(depth);
        if (value == null || quartz == null || !quartz.holdsHandles()) {
            return Mapper.step(value, target, depth);
        }
        Class<?> raw = Mapper.raw(target);
        if (raw == Optional.class && quartz.kind() != Kind.HANDLE) {
            return Walk.parts(depth, 1, i -> toStep(value, quartz, argument(target, 0), handles, d),
                    v -> Optional.ofNullable(v[0]));
        }
        return switch (quartz.kind()) {
            case HANDLE -> handle(value, quartz, target, raw, handles);
            case NULLABLE -> toStep(value, quartz.element(), target, handles, depth);
            case LIST -> value instanceof List<?> l ? list(l, quartz.element(), target, raw, handles, depth)
                    : Mapper.step(value, target, depth);
            case MAP -> value instanceof Map<?, ?> m ? map(m, quartz, target, raw, handles, depth)
                    : Mapper.step(value, target, depth);
            case STRUCT -> value instanceof Struct st ? struct(st, quartz, target, raw, handles, depth)
                    : Mapper.step(value, target, depth);
            case TUPLE -> value instanceof List<?> l ? tuple(l, quartz, target, raw, handles, depth)
                    : Mapper.step(value, target, depth);
            case ENUM -> variant(value, quartz, target, raw, handles, depth);
            default -> Mapper.step(value, target, depth);
        };
    }

    private static Object fromStep(Object value, Type quartz, HandleMapper handles, int depth) {
        int d = Walk.deeper(depth);
        if (value instanceof Value v) {
            value = v.get();
        }
        if (value instanceof Optional<?> o) {
            value = o.orElse(null);
        }
        Kind kind = quartz.kind();
        if (value == null) {
            if (kind == Kind.NULLABLE || kind == Kind.VOID) {
                return null;
            }
            throw new IllegalArgumentException("null cannot be passed as " + describe(quartz));
        }
        if (kind.isInteger()) {
            return Slots.integer(kind, value);
        }
        return switch (kind) {
            case VOID -> null;
            case BOOL -> {
                if (!(value instanceof Boolean)) {
                    throw Slots.mismatch(quartz, value);
                }
                yield value;
            }
            case F64 -> Slots.floating(quartz, value);
            case F32 -> (float) Slots.floating(quartz, value);
            case CHAR -> (int) Slots.codePoint(quartz, value);
            case STR -> {
                if (!(value instanceof String) && !(value instanceof LossyText)) {
                    throw Slots.mismatch(quartz, value);
                }
                yield value;
            }
            case BYTES -> {
                if (!(value instanceof byte[])) {
                    throw Slots.mismatch(quartz, value);
                }
                yield value;
            }
            case HANDLE -> {
                if (value instanceof Long || value instanceof Integer) {
                    yield ((Number) value).longValue();
                }
                if (handles == null) {
                    throw new IllegalArgumentException("a " + value.getClass().getSimpleName() + " cannot be passed as"
                            + " the handle " + describe(quartz) + ": pass its id or give a HandleMapper");
                }
                yield handles.id(value, quartz);
            }
            case LIST -> {
                Object[] items = elements(value, quartz);
                yield Walk.parts(depth, items.length, i -> fromStep(items[i], quartz.element(), handles, d),
                        v -> Collections.unmodifiableList(Arrays.asList(v)));
            }
            case MAP -> {
                if (!(value instanceof Map<?, ?> map)) {
                    throw Slots.mismatch(quartz, value);
                }
                Object[] flat = Mapper.entries(map);
                yield Walk.parts(depth, flat.length,
                        i -> fromStep(flat[i], i % 2 == 0 ? quartz.key() : quartz.element(), handles, d),
                        Mapper::linkedMap);
            }
            case STRUCT -> {
                Object[] values = Slots.structValues(quartz, value);
                List<Type.Field> fields = quartz.fields();
                yield Walk.parts(depth, values.length, i -> fromStep(values[i], fields.get(i).type(), handles, d),
                        v -> new Struct(quartz, v));
            }
            case TUPLE -> {
                Object[] values = Slots.tupleValues(quartz, value);
                yield Walk.parts(depth, values.length,
                        i -> fromStep(values[i], quartz.fields().get(i).type(), handles, d),
                        v -> Collections.unmodifiableList(Arrays.asList(v)));
            }
            case ENUM -> {
                Variant v = Slots.variantOf(quartz, value);
                Type.Case c = quartz.findCase(v.name());
                if (c == null) {
                    throw new IllegalArgumentException("no variant " + v.name() + " in " + describe(quartz)
                            + "; it has " + quartz.cases().stream().map(Type.Case::name).toList());
                }
                if (c.payload().size() != v.values().size()) {
                    throw new IllegalArgumentException("variant " + c.name() + " takes " + c.payload().size()
                            + " values, got " + v.values().size());
                }
                List<Object> payload = v.values();
                yield Walk.parts(depth, payload.size(),
                        i -> fromStep(payload.get(i), c.payload().get(i).type(), handles, d),
                        out -> quartz.isResult() ? (c.index() == 0 ? Result.ok(out[0]) : Result.err(out[0]))
                                : new Variant(c.name(), Arrays.asList(out)));
            }
            case NULLABLE -> fromStep(value, quartz.element(), handles, depth);
            default -> value;
        };
    }

    private static Object handle(Object value, Type quartz, java.lang.reflect.Type target, Class<?> raw,
            HandleMapper handles) {
        if (!(value instanceof Number n)) {
            if (raw.isInstance(value)) {
                return value;
            }
            throw new IllegalArgumentException("a " + Mapper.describe(value) + " is not a handle id");
        }
        long id = n.longValue();
        if (handles == null || raw == long.class || raw == Long.class || raw == Number.class) {
            return Mapper.convert(id, target);
        }
        Object object = handles.object(id, quartz);
        if (raw == Optional.class) {
            return Optional.ofNullable(object);
        }
        if (object != null && !Mapper.box(raw).isInstance(object)) {
            throw new IllegalArgumentException("handle " + id + " is a " + object.getClass().getName() + ", not a "
                    + raw.getName());
        }
        return object;
    }

    private static Object list(List<?> list, Type element, java.lang.reflect.Type target, Class<?> raw,
            HandleMapper handles, int depth) {
        int d = Walk.deeper(depth);
        java.lang.reflect.Type each = raw.isArray() || target instanceof GenericArrayType ? component(target)
                : argument(target, 0);
        if (!raw.isArray() && raw != Object.class && !raw.isAssignableFrom(List.class)
                && !Collection.class.isAssignableFrom(raw)) {
            throw new IllegalArgumentException("a list cannot be read as " + raw.getName());
        }
        Object[] items = list.toArray();
        return Walk.parts(depth, items.length, i -> toStep(items[i], element, each, handles, d), v -> {
            if (!raw.isArray()) {
                return Collections.unmodifiableList(Arrays.asList(v));
            }
            Object array = Array.newInstance(raw.getComponentType(), v.length);
            for (int i = 0; i < v.length; ++i) {
                Array.set(array, i, v[i]);
            }
            return array;
        });
    }

    private static Object map(Map<?, ?> map, Type quartz, java.lang.reflect.Type target, Class<?> raw,
            HandleMapper handles, int depth) {
        int d = Walk.deeper(depth);
        if (raw != Object.class && !raw.isAssignableFrom(LinkedHashMap.class)) {
            throw new IllegalArgumentException("a map cannot be read as " + raw.getName());
        }
        Object[] flat = Mapper.entries(map);
        return Walk.parts(depth, flat.length,
                i -> i % 2 == 0 ? toStep(flat[i], quartz.key(), argument(target, 0), handles, d)
                        : toStep(flat[i], quartz.element(), argument(target, 1), handles, d), Mapper::linkedMap);
    }

    private static Object struct(Struct struct, Type quartz, java.lang.reflect.Type target, Class<?> raw,
            HandleMapper handles, int depth) {
        int d = Walk.deeper(depth);
        Object[] values = struct.values();
        Type[] types = new Type[values.length];
        for (int i = 0; i < values.length; ++i) {
            types[i] = fieldType(quartz, struct.type().fields().get(i));
        }
        if (raw == Object.class || raw == Struct.class) {
            return Walk.parts(depth, values.length, i -> toStep(values[i], types[i], Object.class, handles, d),
                    v -> new Struct(struct.type(), v));
        }
        Shape s = Shape.of(raw);
        if (s.form != Shape.Form.PRODUCT) {
            return Walk.parts(depth, 1, i -> struct(struct, quartz, Object.class, Object.class, handles, d),
                    v -> Mapper.step(v[0], target, depth));
        }
        int[] plan = s.readPlan(struct.type());
        return Walk.parts(depth, plan.length,
                j -> toStep(values[plan[j]], types[plan[j]], s.types[j], handles, d),
                args -> Mapper.construct(s, args));
    }

    private static Object tuple(List<?> values, Type quartz, java.lang.reflect.Type target, Class<?> raw,
            HandleMapper handles, int depth) {
        int d = Walk.deeper(depth);
        List<Type.Field> fields = quartz.fields();
        Shape s = raw == Object.class || List.class.isAssignableFrom(raw) ? null : Shape.of(raw);
        if (s != null && s.form == Shape.Form.PRODUCT) {
            if (s.types.length != values.size()) {
                throw new IllegalArgumentException(raw.getSimpleName() + " has " + s.types.length + " components, "
                        + "the tuple has " + values.size());
            }
            return Walk.parts(depth, values.size(),
                    j -> toStep(values.get(j), fields.get(j).type(), s.types[j], handles, d),
                    args -> Mapper.construct(s, args));
        }
        return Walk.parts(depth, values.size(),
                i -> toStep(values.get(i), fields.get(i).type(), argument(target, 0), handles, d), v -> {
                    List<Object> out = Collections.unmodifiableList(Arrays.asList(v));
                    return s == null ? out : Mapper.step(out, target, depth);
                });
    }

    private static Object variant(Object value, Type quartz, java.lang.reflect.Type target, Class<?> raw,
            HandleMapper handles, int depth) {
        int d = Walk.deeper(depth);
        String name;
        List<?> payload;
        switch (value) {
            case Variant v -> {
                name = v.name();
                payload = v.values();
            }
            case Result.Ok<?, ?> ok -> {
                name = "Ok";
                payload = Collections.singletonList(ok.value());
            }
            case Result.Err<?, ?> err -> {
                name = "Err";
                payload = Collections.singletonList(err.error());
            }
            default -> {
                return Mapper.step(value, target, depth);
            }
        }
        Type.Case c = quartz.findCase(name);
        if (c == null || c.payload().size() != payload.size()) {
            throw new IllegalArgumentException("variant " + name + " does not fit " + describe(quartz));
        }
        if (raw == Result.class || (raw == Object.class && quartz.isResult())) {
            return Walk.parts(depth, 1,
                    i -> toStep(payload.get(0), c.payload().get(0).type(), argument(target, c.index()), handles, d),
                    v -> c.index() == 0 ? Result.ok(v[0]) : Result.err(v[0]));
        }
        if (raw == Object.class || raw == Variant.class) {
            return Walk.parts(depth, payload.size(),
                    i -> toStep(payload.get(i), c.payload().get(i).type(), Object.class, handles, d),
                    v -> new Variant(c.name(), Arrays.asList(v)));
        }
        Shape s = Shape.of(raw);
        Class<?> chosen = raw;
        if (s.form == Shape.Form.SEALED) {
            chosen = Mapper.subclass(s, c.name());
            if (chosen == null) {
                throw new IllegalArgumentException("no subclass of " + raw.getSimpleName() + " is named like the "
                        + "variant " + c.name());
            }
            s = Shape.of(chosen);
        }
        if (s.form == Shape.Form.SINGLETON && payload.isEmpty()) {
            return s.instance;
        }
        if (s.form != Shape.Form.PRODUCT) {
            return Mapper.step(value, target, depth);
        }
        if (s.types.length != payload.size()) {
            throw new IllegalArgumentException(chosen.getSimpleName() + " has " + s.types.length + " components, "
                    + "variant " + c.name() + " carries " + payload.size());
        }
        Shape product = s;
        return Walk.parts(depth, payload.size(),
                j -> toStep(payload.get(j), c.payload().get(j).type(), product.types[j], handles, d),
                args -> Mapper.construct(product, args));
    }

    /** The declared type of {@code field}, looked up by name in {@code quartz} for a struct built with Struct.of. */
    private static Type fieldType(Type quartz, Type.Field field) {
        for (Type.Field f : quartz.fields()) {
            if (f.name().equals(field.name())) {
                return f.type();
            }
        }
        return field.type();
    }

    private static Object[] elements(Object value, Type quartz) {
        if (value instanceof Collection<?> c) {
            return c.toArray();
        }
        if (value instanceof Iterable<?> it) {
            List<Object> out = new ArrayList<>();
            it.forEach(out::add);
            return out.toArray();
        }
        if (value instanceof Object[] array) {
            return array.clone();
        }
        if (value != null && value.getClass().isArray()) {
            Object[] out = new Object[Array.getLength(value)];
            for (int i = 0; i < out.length; ++i) {
                out[i] = Array.get(value, i);
            }
            return out;
        }
        throw Slots.mismatch(quartz, value);
    }

    private static java.lang.reflect.Type argument(java.lang.reflect.Type type, int index) {
        if (type instanceof WildcardType w) {
            return argument(w.getUpperBounds()[0], index);
        }
        if (type instanceof ParameterizedType p && p.getActualTypeArguments().length > index) {
            return p.getActualTypeArguments()[index];
        }
        return Object.class;
    }

    private static java.lang.reflect.Type component(java.lang.reflect.Type type) {
        if (type instanceof GenericArrayType g) {
            return g.getGenericComponentType();
        }
        return type instanceof Class<?> c && c.isArray() ? c.getComponentType() : Object.class;
    }

    private static String describe(Type type) {
        return type.name().isEmpty() ? type.toString() : type.name();
    }
}
