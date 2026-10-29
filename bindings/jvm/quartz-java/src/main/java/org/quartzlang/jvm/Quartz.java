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

    /** The warning rule ids {@link CompileOptions#withRule} accepts ({@code unused-variable}...). */
    public static List<String> warningRules() {
        return Compiler.warningRules();
    }

    /** {@code data} compressed with DEFLATE at {@code level} (0 stores, 1 to 9, clamped) in {@code framing}. */
    public static byte[] deflate(byte[] data, int level, Framing framing) {
        return Compression.deflate(data, level, framing);
    }

    /**
     * {@code data} decompressed, at most {@code maxLength} bytes.
     *
     * @throws QuartzException on a bad header, damaged data, a checksum mismatch or more output than allowed
     */
    public static byte[] inflate(byte[] data, Framing framing, long maxLength) {
        return Compression.inflate(data, framing, maxLength);
    }

    /** CRC-32 as zip and gzip use it, from 0 or continuing a previous result over more data. */
    public static long crc32(long crc, byte[] data) {
        return Compression.crc32(crc, data);
    }

    public static Program compile(Source... sources) {
        return compile(List.of(sources), CompileOptions.DEFAULT);
    }

    /** @throws CompileException with the diagnostics when the sources do not compile */
    public static Program compile(List<Source> sources, CompileOptions options) {
        return Compiler.compile(sources, options);
    }
}
