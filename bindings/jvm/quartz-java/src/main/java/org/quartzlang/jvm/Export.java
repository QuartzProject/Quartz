package org.quartzlang.jvm;

import java.util.List;

/** A function of one program, resolved once so calls skip the lookup and the signature query. */
public final class Export {
    private final Program program;
    private final int index;
    private final String name;
    private final String packageName;
    private final Signature signature;
    private final List<String> parameterNames;
    private final Visibility visibility;
    private final boolean suspend;
    private final boolean exported;
    private final String doc;
    private final int[] offsets;
    private final int argSlots;
    private final boolean simple;
    private volatile List<Attribute> attributes;

    Export(Program program, int index, String name, String packageName, Signature signature,
            List<String> parameterNames, Visibility visibility, boolean suspend, boolean exported, String doc) {
        this.program = program;
        this.index = index;
        this.name = name;
        this.packageName = packageName;
        this.signature = signature;
        this.parameterNames = List.copyOf(parameterNames);
        this.visibility = visibility;
        this.suspend = suspend;
        this.exported = exported;
        this.doc = doc == null ? "" : doc;
        this.offsets = new int[signature.arity()];
        int at = 0;
        boolean scalar = true;
        for (int i = 0; i < offsets.length; ++i) {
            offsets[i] = at;
            Type t = signature.params().get(i);
            at += t.slots();
            scalar &= t.slots() == 1 && !Slots.isComposite(t.kind());
        }
        this.argSlots = at;
        this.simple = scalar;
    }

    public Program program() {
        return program;
    }

    /** The name with its package ({@code org.example.greet}, {@code org.example.Shop.buy}). */
    public String name() {
        return name;
    }

    /** The package ({@code org.example}); empty for a program without one. */
    public String packageName() {
        return packageName;
    }

    /** The name without its package ({@code greet}, {@code Shop.buy}). */
    public String simpleName() {
        return packageName.isEmpty() || !name.startsWith(packageName + ".") ? name
                : name.substring(packageName.length() + 1);
    }

    /** What a caller passes and gets back; calls are checked against it. */
    public Signature signature() {
        return signature;
    }

    /** One name per parameter of {@link #signature()}; empty strings for a function that is not exported. */
    public List<String> parameterNames() {
        return parameterNames;
    }

    /** What the declaration wrote ({@code pub}, {@code private}...). */
    public Visibility visibility() {
        return visibility;
    }

    /** True for a {@code suspend fn}, which runs as a task: {@link Sandbox#spawn(Export, Object...)}. */
    public boolean isSuspend() {
        return suspend;
    }

    /** True when the program offers it to the host ({@link Program#exports()}). */
    public boolean isExported() {
        return exported;
    }

    /** The {@code ///} text of its declaration, as the generated interfaces carry it; empty when it has none. */
    public String doc() {
        return doc;
    }

    /** Its attributes as written ({@code @on}, {@code @command}...), in source order. */
    public List<Attribute> attributes() {
        List<Attribute> known = attributes;
        if (known == null) {
            known = program.attributesOf(index);
            attributes = known;
        }
        return known;
    }

    /** The first attribute called {@code name} (without the {@code @}), or null. */
    public Attribute attribute(String name) {
        for (Attribute a : attributes()) {
            if (a.name().equals(name)) {
                return a;
            }
        }
        return null;
    }

    /** The function's index in the program, as the C interface numbers functions. */
    public int index() {
        return index;
    }

    /** First argument slot of parameter {@code i}. */
    int offset(int i) {
        return offsets[i];
    }

    /** Argument slots a call passes, the sum of every parameter's slots. */
    int argSlots() {
        return argSlots;
    }

    /** True when every parameter is one slot that is not a struct, tuple, enum or nullable. */
    boolean simple() {
        return simple;
    }

    @Override
    public String toString() {
        return "Export[" + name + " " + signature + "]";
    }
}
