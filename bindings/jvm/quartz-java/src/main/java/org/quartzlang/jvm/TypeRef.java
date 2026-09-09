package org.quartzlang.jvm;

import java.lang.reflect.ParameterizedType;

/**
 * A generic type to read a value as, captured by subclassing: {@code new TypeRef<List<Point>>() {}}.
 *
 * @param <T> the type
 */
public abstract class TypeRef<T> {
    private final java.lang.reflect.Type type;

    protected TypeRef() {
        if (!(getClass().getGenericSuperclass() instanceof ParameterizedType p)) {
            throw new IllegalStateException("create a TypeRef as an anonymous subclass with its type argument");
        }
        type = p.getActualTypeArguments()[0];
    }

    public java.lang.reflect.Type type() {
        return type;
    }

    @Override
    public String toString() {
        return "TypeRef<" + type.getTypeName() + ">";
    }
}
