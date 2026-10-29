package org.quartzlang.jvm;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_BYTE;
import static java.lang.foreign.ValueLayout.JAVA_LONG;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.util.Objects;

/** qz_deflate, qz_inflate and qz_crc32: the DEFLATE code the standard library runs, for hosts and kits. */
final class Compression {
    private Compression() {}

    static byte[] deflate(byte[] data, int level, Framing framing) {
        Objects.requireNonNull(framing, "framing");
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment in = copyIn(arena, data);
            MemorySegment length = arena.allocate(JAVA_LONG);
            MemorySegment out = (MemorySegment) Native.DEFLATE.invokeExact(in, (long) data.length, level,
                    framing.ordinal(), length);
            if (out.address() == 0) {
                throw new OutOfMemoryError("libquartz cannot allocate the compressed data");
            }
            return take(out, length.get(JAVA_LONG, 0));
        } catch (Throwable t) {
            throw Native.rethrow(t);
        }
    }

    static byte[] inflate(byte[] data, Framing framing, long maxLength) {
        Objects.requireNonNull(framing, "framing");
        if (maxLength < 0) {
            throw new IllegalArgumentException("maxLength cannot be negative");
        }
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment in = copyIn(arena, data);
            MemorySegment length = arena.allocate(JAVA_LONG);
            MemorySegment error = arena.allocate(ADDRESS);
            MemorySegment out = (MemorySegment) Native.INFLATE.invokeExact(in, (long) data.length, framing.ordinal(),
                    Math.min(maxLength, Integer.MAX_VALUE - 8L), length, error);
            String message = Native.takeString(error.get(ADDRESS, 0), true);
            if (out.address() == 0) {
                throw new QuartzException(message == null ? "cannot decompress the data" : message);
            }
            return take(out, length.get(JAVA_LONG, 0));
        } catch (Throwable t) {
            throw Native.rethrow(t);
        }
    }

    static long crc32(long crc, byte[] data) {
        try {
            int next = (int) Native.CRC32.invokeExact((int) crc, MemorySegment.ofArray(data), (long) data.length);
            return Integer.toUnsignedLong(next);
        } catch (Throwable t) {
            throw Native.rethrow(t);
        }
    }

    private static MemorySegment copyIn(Arena arena, byte[] data) {
        MemorySegment in = arena.allocate(Math.max(1, data.length));
        MemorySegment.copy(data, 0, in, JAVA_BYTE, 0, data.length);
        return in;
    }

    private static byte[] take(MemorySegment out, long length) {
        try {
            return out.reinterpret(length).toArray(JAVA_BYTE);
        } finally {
            Native.free(out);
        }
    }
}
