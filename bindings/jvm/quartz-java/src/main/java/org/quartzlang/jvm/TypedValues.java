package org.quartzlang.jvm;

import java.lang.reflect.Array;
import java.lang.reflect.GenericArrayType;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.WildcardType;
import java.util.ArrayList;
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
        if (value == null || quartz == null || !quartz.holdsHandles()) {
            return Mapper.convert(value, target);
        }
        Class<?> raw = Mapper.raw(target);
        if (raw == Optional.class && quartz.kind() != Kind.HANDLE) {
            return Optional.ofNullable(to(value, quartz, argument(target, 0), handles));
        }
        return switch (quartz.kind()) {
            case HANDLE -> handle(value, quartz, target, raw, handles);
            case NULLABLE -> to(value, quartz.element(), target, handles);
            case LIST -> value instanceof List<?> l ? list(l, quartz.element(), target, raw, handles)
                    : Mapper.convert(value, target);
            case MAP -> value instanceof Map<?, ?> m ? map(m, quartz, target, raw, handles)
                    : Mapper.convert(value, target);
            case STRUCT -> value instanceof Struct st ? struct(st, quartz, target, raw, handles)
                    : Mapper.convert(value, target);
            case TUPLE -> value instanceof List<?> l ? tuple(l, quartz, target, raw, handles)
                    : Mapper.convert(value, target);
            case ENUM -> variant(value, quartz, target, raw, handles);
            default -> Mapper.convert(value, target);
        };
    }

    /** {@code value} as the generic form of {@code quartz}: what {@link Value#get()} would give for it. */
    static Object from(Object value, Type quartz, HandleMapper handles) {
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
                if (!(value instanceof String)) {
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
                List<Object> out = new ArrayList<>();
                for (Object e : elements(value, quartz)) {
                    out.add(from(e, quartz.element(), handles));
                }
                yield Collections.unmodifiableList(out);
            }
            case MAP -> {
                if (!(value instanceof Map<?, ?> map)) {
                    throw Slots.mismatch(quartz, value);
                }
                Map<Object, Object> out = new LinkedHashMap<>();
                for (Map.Entry<?, ?> e : map.entrySet()) {
                    out.put(from(e.getKey(), quartz.key(), handles), from(e.getValue(), quartz.element(), handles));
                }
                yield Collections.unmodifiableMap(out);
            }
            case STRUCT -> {
                Object[] values = Slots.structValues(quartz, value);
                List<Type.Field> fields = quartz.fields();
                Object[] out = new Object[values.length];
                for (int i = 0; i < out.length; ++i) {
                    out[i] = from(values[i], fields.get(i).type(), handles);
                }
                yield new Struct(quartz, out);
            }
            case TUPLE -> {
                Object[] values = Slots.tupleValues(quartz, value);
                List<Object> out = new ArrayList<>(values.length);
                for (int i = 0; i < values.length; ++i) {
                    out.add(from(values[i], quartz.fields().get(i).type(), handles));
                }
                yield Collections.unmodifiableList(out);
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
                List<Object> out = new ArrayList<>(c.payload().size());
                for (int i = 0; i < c.payload().size(); ++i) {
                    out.add(from(v.values().get(i), c.payload().get(i).type(), handles));
                }
                if (quartz.isResult()) {
                    yield c.index() == 0 ? Result.ok(out.get(0)) : Result.err(out.get(0));
                }
                yield new Variant(c.name(), out);
            }
            case NULLABLE -> from(value, quartz.element(), handles);
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
            HandleMapper handles) {
        java.lang.reflect.Type each = raw.isArray() || target instanceof GenericArrayType ? component(target)
                : argument(target, 0);
        List<Object> out = new ArrayList<>(list.size());
        for (Object e : list) {
            out.add(to(e, element, each, handles));
        }
        if (raw.isArray()) {
            Object array = Array.newInstance(raw.getComponentType(), out.size());
            for (int i = 0; i < out.size(); ++i) {
                Array.set(array, i, out.get(i));
            }
            return array;
        }
        if (raw == Object.class || raw.isAssignableFrom(List.class) || Collection.class.isAssignableFrom(raw)) {
            return Collections.unmodifiableList(out);
        }
        throw new IllegalArgumentException("a list cannot be read as " + raw.getName());
    }

    private static Object map(Map<?, ?> map, Type quartz, java.lang.reflect.Type target, Class<?> raw,
            HandleMapper handles) {
        if (raw != Object.class && !raw.isAssignableFrom(LinkedHashMap.class)) {
            throw new IllegalArgumentException("a map cannot be read as " + raw.getName());
        }
        Map<Object, Object> out = new LinkedHashMap<>();
        for (Map.Entry<?, ?> e : map.entrySet()) {
            out.put(to(e.getKey(), quartz.key(), argument(target, 0), handles),
                    to(e.getValue(), quartz.element(), argument(target, 1), handles));
        }
        return Collections.unmodifiableMap(out);
    }

    private static Object struct(Struct struct, Type quartz, java.lang.reflect.Type target, Class<?> raw,
            HandleMapper handles) {
        Object[] values = struct.values();
        Type[] types = new Type[values.length];
        for (int i = 0; i < values.length; ++i) {
            types[i] = fieldType(quartz, struct.type().fields().get(i));
        }
        if (raw == Object.class || raw == Struct.class) {
            Object[] out = new Object[values.length];
            for (int i = 0; i < out.length; ++i) {
                out[i] = to(values[i], types[i], Object.class, handles);
            }
            return new Struct(struct.type(), out);
        }
        Shape s = Shape.of(raw);
        if (s.form != Shape.Form.PRODUCT) {
            return Mapper.convert(struct(struct, quartz, Object.class, Object.class, handles), target);
        }
        int[] plan = s.readPlan(struct.type());
        Object[] args = new Object[plan.length];
        for (int j = 0; j < args.length; ++j) {
            args[j] = to(values[plan[j]], types[plan[j]], s.types[j], handles);
        }
        return construct(s, args);
    }

    private static Object tuple(List<?> values, Type quartz, java.lang.reflect.Type target, Class<?> raw,
            HandleMapper handles) {
        List<Type.Field> fields = quartz.fields();
        Shape s = raw == Object.class || List.class.isAssignableFrom(raw) ? null : Shape.of(raw);
        if (s != null && s.form == Shape.Form.PRODUCT) {
            if (s.types.length != values.size()) {
                throw new IllegalArgumentException(raw.getSimpleName() + " has " + s.types.length + " components, "
                        + "the tuple has " + values.size());
            }
            Object[] args = new Object[values.size()];
            for (int j = 0; j < args.length; ++j) {
                args[j] = to(values.get(j), fields.get(j).type(), s.types[j], handles);
            }
            return construct(s, args);
        }
        List<Object> out = new ArrayList<>(values.size());
        for (int i = 0; i < values.size(); ++i) {
            out.add(to(values.get(i), fields.get(i).type(), argument(target, 0), handles));
        }
        return s == null ? Collections.unmodifiableList(out) : Mapper.convert(out, target);
    }

    private static Object variant(Object value, Type quartz, java.lang.reflect.Type target, Class<?> raw,
            HandleMapper handles) {
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
                return Mapper.convert(value, target);
            }
        }
        Type.Case c = quartz.findCase(name);
        if (c == null || c.payload().size() != payload.size()) {
            throw new IllegalArgumentException("variant " + name + " does not fit " + describe(quartz));
        }
        if (raw == Result.class || (raw == Object.class && quartz.isResult())) {
            Object one = to(payload.get(0), c.payload().get(0).type(), argument(target, c.index()), handles);
            return c.index() == 0 ? Result.ok(one) : Result.err(one);
        }
        if (raw == Object.class || raw == Variant.class) {
            List<Object> out = new ArrayList<>(payload.size());
            for (int i = 0; i < payload.size(); ++i) {
                out.add(to(payload.get(i), c.payload().get(i).type(), Object.class, handles));
            }
            return new Variant(c.name(), out);
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
            return Mapper.convert(value, target);
        }
        if (s.types.length != payload.size()) {
            throw new IllegalArgumentException(chosen.getSimpleName() + " has " + s.types.length + " components, "
                    + "variant " + c.name() + " carries " + payload.size());
        }
        Object[] args = new Object[payload.size()];
        for (int j = 0; j < args.length; ++j) {
            args[j] = to(payload.get(j), c.payload().get(j).type(), s.types[j], handles);
        }
        return construct(s, args);
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

    private static Object construct(Shape s, Object[] args) {
        try {
            return s.construct(args);
        } catch (Throwable t) {
            throw new IllegalArgumentException("cannot build " + s.type.getSimpleName() + ": " + t, t);
        }
    }

    private static Iterable<?> elements(Object value, Type quartz) {
        if (value instanceof Iterable<?> it) {
            return it;
        }
        if (value instanceof Object[] array) {
            return java.util.Arrays.asList(array);
        }
        if (value != null && value.getClass().isArray()) {
            List<Object> out = new ArrayList<>();
            for (int i = 0; i < Array.getLength(value); ++i) {
                out.add(Array.get(value, i));
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
