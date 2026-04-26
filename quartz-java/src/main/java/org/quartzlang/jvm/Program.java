package org.quartzlang.jvm;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_BYTE;
import static java.lang.foreign.ValueLayout.JAVA_LONG;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.ref.Cleaner;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.NoSuchElementException;
import java.util.Optional;
import java.util.concurrent.ConcurrentHashMap;

/** A compiled or loaded program, immutable and shareable; freed once closed and no sandbox uses it. */
public final class Program implements AutoCloseable {
    private final NativeHandle handle;
    private final Cleaner.Cleanable cleanable;
    private final String diagnostics;
    private final ConcurrentHashMap<String, Optional<Export>> exports = new ConcurrentHashMap<>();

    Program(MemorySegment address, String diagnostics) {
        this.handle = new NativeHandle(address, Program::free);
        this.cleanable = Cleaning.CLEANER.register(this, handle::release);
        this.diagnostics = diagnostics == null ? "" : diagnostics;
    }

    /** Loads a .qzb module, checking its hash and decoding and validating it. */
    public static Program load(byte[] bytes) {
        return load(bytes, List.of(), false);
    }

    /** Loads a .qzb module; with trusted Ed25519 public keys (32 bytes each) its signature is checked too. */
    public static Program load(byte[] bytes, List<byte[]> trustedKeys, boolean requireSignature) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment module = arena.allocate(Math.max(1, bytes.length));
            MemorySegment.copy(bytes, 0, module, JAVA_BYTE, 0, bytes.length);
            MemorySegment keys = MemorySegment.NULL;
            if (!trustedKeys.isEmpty()) {
                keys = arena.allocate(32L * trustedKeys.size());
                for (int i = 0; i < trustedKeys.size(); ++i) {
                    byte[] key = trustedKeys.get(i);
                    if (key.length != 32) {
                        throw new IllegalArgumentException("an Ed25519 public key has 32 bytes");
                    }
                    MemorySegment.copy(key, 0, keys, JAVA_BYTE, 32L * i, 32);
                }
            }
            MemorySegment error = arena.allocate(ADDRESS);
            MemorySegment program = (MemorySegment) Native.LOAD.invokeExact(module, (long) bytes.length, keys,
                    (long) trustedKeys.size(), requireSignature, error);
            String message = Native.takeString(error.get(ADDRESS, 0), true);
            if (program.address() == 0) {
                throw new QuartzException(message == null ? "cannot load the module" : message);
            }
            return new Program(program, "");
        } catch (Throwable t) {
            throw Native.rethrow(t);
        }
    }

    /** Warnings the compiler printed; empty for a loaded module. */
    public String diagnostics() {
        return diagnostics;
    }

    /** The portable .qzb bytes, present when compiled with {@link CompileOptions#emitBinary()} or loaded. */
    public Optional<byte[]> binary() {
        handle.retain();
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment length = arena.allocate(JAVA_LONG);
            MemorySegment bytes = (MemorySegment) Native.PROGRAM_BINARY.invokeExact(handle.address(), length);
            if (bytes.address() == 0) {
                return Optional.empty();
            }
            try {
                return Optional.of(bytes.reinterpret(length.get(JAVA_LONG, 0)).toArray(JAVA_BYTE));
            } finally {
                Native.free(bytes);
            }
        } catch (Throwable t) {
            throw Native.rethrow(t);
        } finally {
            handle.release();
        }
    }

    /** The function called {@code name}, if the program has one. */
    public Optional<Export> find(String name) {
        return exports.computeIfAbsent(name, this::lookup);
    }

    /** @throws NoSuchElementException when the program has no function called {@code name} */
    public Export export(String name) {
        return find(name).orElseThrow(() -> new NoSuchElementException("no function named " + name));
    }

    private Optional<Export> lookup(String name) {
        handle.retain();
        try (Arena arena = Arena.ofConfined()) {
            int index = (int) Native.PROGRAM_FUNCTION.invokeExact(handle.address(),
                    arena.allocateFrom(name, StandardCharsets.UTF_8));
            return index == -1 ? Optional.empty() : Optional.of(new Export(this, name, index));
        } catch (Throwable t) {
            throw Native.rethrow(t);
        } finally {
            handle.release();
        }
    }

    NativeHandle handle() {
        return handle;
    }

    /** Releases this program; sandboxes made from it keep working. Idempotent. */
    @Override
    public void close() {
        cleanable.clean();
    }

    private static void free(MemorySegment address) {
        try {
            Native.PROGRAM_FREE.invokeExact(address);
        } catch (Throwable t) {
            throw Native.rethrow(t);
        }
    }
}
