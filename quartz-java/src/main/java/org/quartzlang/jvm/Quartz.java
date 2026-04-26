package org.quartzlang.jvm;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_BOOLEAN;
import static java.lang.foreign.ValueLayout.JAVA_BYTE;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.nio.charset.StandardCharsets;
import java.util.List;

/** Entry point: compiles Quartz sources into a {@link Program}. */
public final class Quartz {
    private Quartz() {}

    /** The ABI version of the loaded libquartz. */
    public static int abiVersion() {
        return Native.abiVersion();
    }

    public static Program compile(Source... sources) {
        return compile(List.of(sources), CompileOptions.DEFAULT);
    }

    /** @throws CompileException with the diagnostics when the sources do not compile */
    public static Program compile(List<Source> sources, CompileOptions options) {
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
            MemorySegment diagnostics = arena.allocate(ADDRESS);
            MemorySegment program = (MemorySegment) Native.COMPILE.invokeExact(array, (long) sources.size(), opts,
                    diagnostics);
            String text = Native.takeString(diagnostics.get(ADDRESS, 0), true);
            if (program.address() == 0) {
                throw new CompileException(text);
            }
            return new Program(program, text);
        } catch (Throwable t) {
            throw Native.rethrow(t);
        }
    }
}
