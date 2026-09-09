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
