package org.quartzlang.jvm;

import java.util.List;

/** Entry point: compiles Quartz sources into a {@link Program}. */
public final class Quartz {
    private Quartz() {}

    /** The ABI version of the loaded libquartz. */
    public static int abiVersion() {
        return Native.abiVersion();
    }

    /** The API level of the loaded libquartz, at least the one these bindings need. */
    public static int apiLevel() {
        return Native.apiLevel();
    }

    /** The warning rule ids {@link CompileOptions#withRule} accepts ({@code unused-variable}, {@code deprecated}...). */
    public static List<String> warningRules() {
        return Compiler.warningRules();
    }

    public static Program compile(Source... sources) {
        return compile(List.of(sources), CompileOptions.DEFAULT);
    }

    /** @throws CompileException with the diagnostics when the sources do not compile */
    public static Program compile(List<Source> sources, CompileOptions options) {
        return Compiler.compile(sources, options);
    }
}
