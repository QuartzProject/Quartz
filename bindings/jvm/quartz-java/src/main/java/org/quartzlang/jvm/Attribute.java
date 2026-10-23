package org.quartzlang.jvm;

import java.util.List;
import java.util.Objects;
import java.util.Optional;

/**
 * An attribute of a function as the source wrote it ({@code @command("heal", perm = "x.heal")}, {@code @on(Join)}),
 * for hosts that register handlers from them.
 *
 * @param name without the {@code @}
 * @param arguments positional ones first as written, then named ones, in source order
 * @param filter for {@code @on(..., where = expr)}, the exported function that tests the handler's first argument;
 *     null otherwise
 */
public record Attribute(String name, List<Argument> arguments, Export filter) {
    /** The type of an argument's value, in the order of {@code qz_attribute_arg_kind}. */
    public enum ArgumentKind {
        INT,
        FLOAT,
        BOOL,
        STR,
        /** A dotted name ({@code Join}, {@code org.example.Join}), given as its text. */
        NAME;

        static ArgumentKind of(int code) {
            return code >= 0 && code < 5 ? values()[code] : NAME;
        }
    }

    /**
     * One argument of an attribute.
     *
     * @param key the name of a named argument; empty for a positional one
     * @param kind what {@code value} holds
     * @param value a Long (INT), Double (FLOAT), Boolean (BOOL) or String (STR, NAME)
     */
    public record Argument(String key, ArgumentKind kind, Object value) {
        public Argument {
            Objects.requireNonNull(key, "key");
            Objects.requireNonNull(kind, "kind");
            Objects.requireNonNull(value, "value");
        }

        public boolean isPositional() {
            return key.isEmpty();
        }

        public long asLong() {
            return as(Long.class, ArgumentKind.INT);
        }

        public double asDouble() {
            return kind == ArgumentKind.INT ? asLong() : as(Double.class, ArgumentKind.FLOAT);
        }

        public boolean asBoolean() {
            return as(Boolean.class, ArgumentKind.BOOL);
        }

        /** The text of a STR or NAME argument. */
        public String asString() {
            if (kind != ArgumentKind.STR && kind != ArgumentKind.NAME) {
                throw new IllegalStateException("argument " + this + " is " + kind + ", not text");
            }
            return (String) value;
        }

        private <T> T as(Class<T> type, ArgumentKind wanted) {
            if (kind != wanted) {
                throw new IllegalStateException("argument " + this + " is " + kind + ", not " + wanted);
            }
            return type.cast(value);
        }

        @Override
        public String toString() {
            String text = kind == ArgumentKind.STR ? "\"" + value + "\"" : String.valueOf(value);
            return key.isEmpty() ? text : key + " = " + text;
        }
    }

    public Attribute {
        Objects.requireNonNull(name, "name");
        arguments = List.copyOf(arguments);
    }

    /** Positional argument {@code index}, if there is one. */
    public Optional<Argument> positional(int index) {
        int i = 0;
        for (Argument a : arguments) {
            if (a.isPositional() && i++ == index) {
                return Optional.of(a);
            }
        }
        return Optional.empty();
    }

    /** The argument named {@code key}, if there is one. */
    public Optional<Argument> named(String key) {
        for (Argument a : arguments) {
            if (a.key().equals(key)) {
                return Optional.of(a);
            }
        }
        return Optional.empty();
    }

    @Override
    public String toString() {
        StringBuilder out = new StringBuilder("@").append(name);
        if (!arguments.isEmpty() || filter != null) {
            out.append('(');
            for (int i = 0; i < arguments.size(); ++i) {
                out.append(i == 0 ? "" : ", ").append(arguments.get(i));
            }
            if (filter != null) {
                out.append(arguments.isEmpty() ? "" : ", ").append("where = ").append(filter.name());
            }
            out.append(')');
        }
        return out.toString();
    }
}
