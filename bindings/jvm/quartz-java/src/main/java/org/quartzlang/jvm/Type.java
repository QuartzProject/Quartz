package org.quartzlang.jvm;

import java.util.List;
import java.util.Locale;
import java.util.Objects;

/** A parameter, result or nested type as the C interface describes it, with the layout of composite types. */
public final class Type {
    /**
     * A field of a struct, an element of a tuple or a value of an enum variant.
     *
     * @param name the field name; empty for tuple elements and variant payloads
     * @param type its type
     * @param offset its first slot, counted from the start of the enclosing value
     */
    public record Field(String name, Type type, int offset) {}

    /**
     * A variant of an enum type.
     *
     * @param name as the program spells it
     * @param index the value of slot 0 for this variant
     * @param payload the values it carries, positional
     */
    public record Case(String name, int index, List<Field> payload) {}

    private final Kind kind;
    private final int slots;
    private final int id;
    private Type element;
    private Type key;
    private List<Field> fields = List.of();
    private List<Case> cases = List.of();
    private boolean pointerNullable;
    private String name = "";

    public Type(Kind kind, int slots) {
        this(kind, slots, 0, null);
    }

    public Type(Kind kind, int slots, int id, Type element) {
        this.kind = Objects.requireNonNull(kind, "kind");
        this.slots = slots;
        this.id = id;
        this.element = element;
        this.pointerNullable = kind == Kind.NULLABLE && slots == 1;
    }

    /** Filled in once by {@link Program} after the type is cached, so recursive types resolve. */
    void complete(Type element, Type key, List<Field> fields, List<Case> cases, boolean pointerNullable) {
        this.element = element;
        this.key = key;
        this.fields = List.copyOf(fields);
        this.cases = List.copyOf(cases);
        this.pointerNullable = pointerNullable;
    }

    void named(String typeName) {
        name = typeName == null ? "" : typeName;
    }

    /** How the source names a struct or enum ({@code Point}, {@code Box<int>}); empty when unknown. */
    public String name() {
        return name;
    }

    public Kind kind() {
        return kind;
    }

    /** How many value slots it takes (a struct takes the sum of its fields). */
    public int slots() {
        return slots;
    }

    /** The program's type id, 0 when unknown. */
    public int id() {
        return id;
    }

    /** The element of a list or nullable, the value of a map, or null. */
    public Type element() {
        return element;
    }

    /** The key of a map, or null. */
    public Type key() {
        return key;
    }

    /** Fields of a struct or elements of a tuple, empty for other kinds. */
    public List<Field> fields() {
        return fields;
    }

    /** Variants of an enum, empty for other kinds. */
    public List<Case> cases() {
        return cases;
    }

    /** The variant called {@code name}, matched exactly or else ignoring case and underscores; null when none. */
    public Case findCase(String name) {
        for (Case c : cases) {
            if (c.name().equals(name)) {
                return c;
            }
        }
        String wanted = Names.normalize(name);
        for (Case c : cases) {
            if (Names.normalize(c.name()).equals(wanted)) {
                return c;
            }
        }
        return null;
    }

    /** A nullable that is one slot, 0 when null (str, Bytes, lists, maps); other nullables add a presence slot. */
    public boolean isPointerNullable() {
        return kind == Kind.NULLABLE && pointerNullable;
    }

    /** A one-slot nullable str, Bytes or list, which is null or a pointer. */
    public boolean isNullableReference() {
        return kind == Kind.NULLABLE && slots == 1 && element != null && element.kind.isReference();
    }

    /** An enum shaped like {@code Result<T, E>}: variants {@code Ok} and {@code Err} with one value each. */
    public boolean isResult() {
        return kind == Kind.ENUM && cases.size() == 2 && cases.get(0).name().equals("Ok")
                && cases.get(1).name().equals("Err") && cases.get(0).payload().size() == 1
                && cases.get(1).payload().size() == 1;
    }

    @Override
    public boolean equals(Object o) {
        if (this == o) {
            return true;
        }
        if (!(o instanceof Type t) || t.kind != kind || t.slots != slots || !Objects.equals(t.element, element)
                || !Objects.equals(t.key, key) || t.fields.size() != fields.size() || t.cases.size() != cases.size()) {
            return false;
        }
        for (int i = 0; i < fields.size(); ++i) {
            Field a = fields.get(i);
            Field b = t.fields.get(i);
            if (!a.name().equals(b.name()) || a.type().kind != b.type().kind || a.offset() != b.offset()) {
                return false;
            }
        }
        for (int i = 0; i < cases.size(); ++i) {
            if (!cases.get(i).name().equals(t.cases.get(i).name())) {
                return false;
            }
        }
        return true;
    }

    @Override
    public int hashCode() {
        return kind.hashCode() * 31 + slots;
    }

    @Override
    public String toString() {
        String name = kind.name().toLowerCase(Locale.ROOT);
        String inner = element == null ? "" : "<" + (key == null ? "" : key + ", ") + element + ">";
        return slots == 1 ? name + inner : name + inner + "[" + slots + "]";
    }
}
