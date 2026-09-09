package org.quartzlang.jvm;

/** A function of one program, resolved by name once so calls skip the lookup and the signature query. */
public final class Export {
    private final Program program;
    private final String name;
    private final int index;
    private final Signature signature;
    private final int[] offsets;
    private final int argSlots;
    private final boolean simple;

    Export(Program program, String name, int index, Signature signature) {
        this.program = program;
        this.name = name;
        this.index = index;
        this.signature = signature;
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

    public String name() {
        return name;
    }

    /** What a caller passes and gets back; calls are checked against it. */
    public Signature signature() {
        return signature;
    }

    int index() {
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
