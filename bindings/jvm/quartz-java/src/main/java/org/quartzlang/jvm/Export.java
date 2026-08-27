package org.quartzlang.jvm;

/** A function of one program, resolved by name once so calls skip the lookup and the signature query. */
public final class Export {
    private final Program program;
    private final String name;
    private final int index;
    private final Signature signature;

    Export(Program program, String name, int index, Signature signature) {
        this.program = program;
        this.name = name;
        this.index = index;
        this.signature = signature;
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

    @Override
    public String toString() {
        return "Export[" + name + " " + signature + "]";
    }
}
