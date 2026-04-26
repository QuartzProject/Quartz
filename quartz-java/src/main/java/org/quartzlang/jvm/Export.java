package org.quartzlang.jvm;

/** A function of one program, resolved by name once so calls skip the lookup. */
public final class Export {
    private final Program program;
    private final String name;
    private final int index;

    Export(Program program, String name, int index) {
        this.program = program;
        this.name = name;
        this.index = index;
    }

    public Program program() {
        return program;
    }

    public String name() {
        return name;
    }

    int index() {
        return index;
    }

    @Override
    public String toString() {
        return "Export[" + name + "]";
    }
}
