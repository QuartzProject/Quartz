package org.quartzlang.jvm;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_BOOLEAN;
import static java.lang.foreign.ValueLayout.JAVA_BYTE;
import static java.lang.foreign.ValueLayout.JAVA_INT;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/** qz_compile_with and the diagnostics it reports. */
final class Compiler {
    private Compiler() {}

    static Program compile(List<Source> sources, CompileOptions options) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment array = arena.allocate(Native.SOURCE, Math.max(1, sources.size()));
            for (int i = 0; i < sources.size(); ++i) {
                Source source = sources.get(i);
                byte[] text = source.text().getBytes(StandardCharsets.UTF_8);
                MemorySegment textSegment = arena.allocate(Math.max(1, text.length));
                MemorySegment.copy(text, 0, textSegment, JAVA_BYTE, 0, text.length);
                MemorySegment entry = array.asSlice(i * Native.SOURCE.byteSize(), Native.SOURCE);
                entry.set(ADDRESS, 0, arena.allocateFrom(source.name(), StandardCharsets.UTF_8));
                entry.set(ADDRESS, ADDRESS.byteSize(), textSegment);
                entry.set(Native.SIZE_T, 2 * ADDRESS.byteSize(), (long) text.length);
            }
            MemorySegment opts = arena.allocate(Native.COMPILE_OPTIONS);
            opts.set(JAVA_BOOLEAN, 0, options.emitBinary());
            opts.set(JAVA_BOOLEAN, 1, options.keepLines());
            opts.set(ADDRESS, 8, options.manifest() == null ? MemorySegment.NULL
                    : arena.allocateFrom(options.manifest(), StandardCharsets.UTF_8));
            Map<String, WarningLevel> rules = options.rules();
            MemorySegment levels = arena.allocate(Native.RULE_LEVEL, Math.max(1, rules.size()));
            int i = 0;
            for (Map.Entry<String, WarningLevel> e : rules.entrySet()) {
                long at = i++ * Native.RULE_LEVEL.byteSize();
                levels.set(ADDRESS, at, arena.allocateFrom(e.getKey(), StandardCharsets.UTF_8));
                levels.set(JAVA_INT, at + ADDRESS.byteSize(), e.getValue().ordinal());
            }
            MemorySegment out = arena.allocate(ADDRESS);
            MemorySegment program = (MemorySegment) Native.COMPILE_WITH.invokeExact(array, (long) sources.size(), opts,
                    levels, (long) rules.size(), out);
            MemorySegment diagnostics = out.get(ADDRESS, 0);
            String text;
            List<Diagnostic> list;
            try {
                text = diagnostics.address() == 0 ? ""
                        : Native.borrowString((MemorySegment) Native.DIAGNOSTICS_TEXT.invokeExact(diagnostics));
                list = read(diagnostics, arena);
            } finally {
                if (diagnostics.address() != 0) {
                    Native.DIAGNOSTICS_FREE.invokeExact(diagnostics);
                }
            }
            if (program.address() == 0) {
                throw new CompileException(text, list);
            }
            return new Program(program, text, list);
        } catch (Throwable t) {
            throw Native.rethrow(t);
        }
    }

    /** The warning rule ids the loaded library knows. */
    static List<String> warningRules() {
        try {
            long count = (long) Native.WARNING_RULE_COUNT.invokeExact();
            List<String> out = new ArrayList<>((int) count);
            for (long i = 0; i < count; ++i) {
                out.add(Native.borrowString((MemorySegment) Native.WARNING_RULE.invokeExact(i)));
            }
            return List.copyOf(out);
        } catch (Throwable t) {
            throw Native.rethrow(t);
        }
    }

    private static List<Diagnostic> read(MemorySegment d, Arena arena) throws Throwable {
        if (d.address() == 0) {
            return List.of();
        }
        long count = (long) Native.DIAGNOSTICS_COUNT.invokeExact(d);
        MemorySegment cells = arena.allocate(JAVA_INT, 4);
        List<Diagnostic> out = new ArrayList<>((int) count);
        for (long i = 0; i < count; ++i) {
            Native.DIAGNOSTIC_RANGE.invokeExact(d, i, cells, cells.asSlice(4), cells.asSlice(8), cells.asSlice(12));
            out.add(new Diagnostic(Diagnostic.Severity.of((int) Native.DIAGNOSTIC_SEVERITY.invokeExact(d, i)),
                    Native.borrowString((MemorySegment) Native.DIAGNOSTIC_RULE.invokeExact(d, i)),
                    Native.borrowString((MemorySegment) Native.DIAGNOSTIC_FILE.invokeExact(d, i)),
                    cells.getAtIndex(JAVA_INT, 0), cells.getAtIndex(JAVA_INT, 1), cells.getAtIndex(JAVA_INT, 2),
                    cells.getAtIndex(JAVA_INT, 3),
                    Native.borrowString((MemorySegment) Native.DIAGNOSTIC_MESSAGE.invokeExact(d, i))));
        }
        return List.copyOf(out);
    }
}
