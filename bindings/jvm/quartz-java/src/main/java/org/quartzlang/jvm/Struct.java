package org.quartzlang.jvm;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.NoSuchElementException;

/** A Quartz struct copied out of a sandbox: its fields by name, in declaration order. Pass a Map to build one. */
public final class Struct {
    private final Type type;
    private final Object[] values;

    Struct(Type type, Object[] values) {
        this.type = type;
        this.values = values;
    }

    /** A struct to pass, with these fields in this order; a struct parameter takes it by name, a tuple by position. */
    public static Struct of(Map<String, ?> fields) {
        return of("", fields);
    }

    /** A struct to pass named like the Quartz struct it is meant for, which is then checked. */
    public static Struct of(String name, Map<String, ?> fields) {
        List<Type.Field> layout = new ArrayList<>(fields.size());
        Object[] values = new Object[fields.size()];
        int i = 0;
        for (Map.Entry<String, ?> e : fields.entrySet()) {
            layout.add(new Type.Field(e.getKey(), new Type(Kind.OTHER, 1), i));
            values[i++] = e.getValue();
        }
        Type type = new Type(Kind.STRUCT, values.length);
        type.complete(null, null, layout, List.of(), false);
        type.named(name);
        return new Struct(type, values);
    }

    public Type type() {
        return type;
    }

    public int size() {
        return values.length;
    }

    /** Field names in declaration order. */
    public List<String> names() {
        List<String> out = new ArrayList<>(values.length);
        for (Type.Field f : type.fields()) {
            out.add(f.name());
        }
        return out;
    }

    /** Field {@code index} in declaration order. */
    public Object get(int index) {
        return values[index];
    }

    /** @throws NoSuchElementException when the struct has no field called {@code name} */
    public Object get(String name) {
        int index = indexOf(name);
        if (index < 0) {
            throw new NoSuchElementException("the struct has no field " + name + "; it has " + names());
        }
        return values[index];
    }

    public boolean has(String name) {
        return indexOf(name) >= 0;
    }

    /** The fields as an unmodifiable map in declaration order. */
    public Map<String, Object> toMap() {
        Map<String, Object> out = new LinkedHashMap<>();
        List<Type.Field> fields = type.fields();
        for (int i = 0; i < values.length; ++i) {
            out.put(fields.get(i).name(), values[i]);
        }
        return Collections.unmodifiableMap(out);
    }

    /** Converts it to {@code target}, a record or Kotlin data class matched by field name. */
    public <T> T to(Class<T> target) {
        return target.cast(Mapper.convert(this, target));
    }

    Object[] values() {
        return values;
    }

    int indexOf(String name) {
        List<Type.Field> fields = type.fields();
        for (int i = 0; i < fields.size(); ++i) {
            if (fields.get(i).name().equals(name)) {
                return i;
            }
        }
        return -1;
    }

    @Override
    public boolean equals(Object o) {
        return o instanceof Struct s && s.names().equals(names()) && Arrays.deepEquals(s.values, values);
    }

    @Override
    public int hashCode() {
        return names().hashCode() * 31 + Arrays.deepHashCode(values);
    }

    @Override
    public String toString() {
        StringBuilder out = new StringBuilder("{");
        List<Type.Field> fields = type.fields();
        for (int i = 0; i < values.length; ++i) {
            out.append(i == 0 ? "" : ", ").append(fields.get(i).name()).append('=');
            out.append(values[i] instanceof byte[] b ? Arrays.toString(b) : String.valueOf(values[i]));
        }
        return out.append('}').toString();
    }
}
