package org.quartzlang.jvm;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_BYTE;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.ref.Cleaner;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
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
    private volatile List<Import> imports;

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
            if (index == -1) {
                return Optional.empty();
            }
            MemorySegment slots = arena.allocate(JAVA_INT);
            MemorySegment p = handle.address();
            int arity = (int) Native.FUNCTION_ARITY.invokeExact(p, index);
            List<Type> params = new ArrayList<>(arity);
            for (int i = 0; i < arity; ++i) {
                int kind = (int) Native.FUNCTION_PARAM.invokeExact(p, index, i, slots);
                params.add(new Type(Kind.of(kind), slots.get(JAVA_INT, 0)));
            }
            int result = (int) Native.FUNCTION_RESULT.invokeExact(p, index, slots);
            return Optional.of(new Export(this, name, index,
                    new Signature(params, new Type(Kind.of(result), slots.get(JAVA_INT, 0)))));
        } catch (Throwable t) {
            throw Native.rethrow(t);
        } finally {
            handle.release();
        }
    }

    /** The host functions this program imports, which a sandbox binds before {@link Sandbox#start()}. */
    public List<Import> imports() {
        List<Import> known = imports;
        if (known != null) {
            return known;
        }
        handle.retain();
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment slots = arena.allocate(JAVA_INT);
            MemorySegment p = handle.address();
            int count = (int) Native.IMPORT_COUNT.invokeExact(p);
            List<Import> out = new ArrayList<>(count);
            for (int i = 0; i < count; ++i) {
                String name = Native.takeString((MemorySegment) Native.IMPORT_NAME.invokeExact(p, i), false);
                int arity = (int) Native.IMPORT_ARITY.invokeExact(p, i);
                List<Type> params = new ArrayList<>(arity);
                for (int k = 0; k < arity; ++k) {
                    int kind = (int) Native.IMPORT_PARAM.invokeExact(p, i, k, slots);
                    params.add(new Type(Kind.of(kind), slots.get(JAVA_INT, 0)));
                }
                int result = (int) Native.IMPORT_RESULT.invokeExact(p, i, slots);
                out.add(new Import(name, new Signature(params, new Type(Kind.of(result), slots.get(JAVA_INT, 0)))));
            }
            known = List.copyOf(out);
            imports = known;
            return known;
        } catch (Throwable t) {
            throw Native.rethrow(t);
        } finally {
            handle.release();
        }
    }

    /** The import called {@code name}, if the program has one. */
    public Optional<Import> findImport(String name) {
        return imports().stream().filter(i -> i.name().equals(name)).findFirst();
    }

    /** The .qzi other packages compile against, for a program built with a package.toml. */
    public String packageInterface() {
        return packageInterface(null);
    }

    /** The .qzi other packages compile against, describing the package with {@code manifest} (package.toml). */
    public String packageInterface(String manifest) {
        handle.retain();
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment error = arena.allocate(ADDRESS);
            MemorySegment m = manifest == null ? MemorySegment.NULL : arena.allocateFrom(manifest,
                    StandardCharsets.UTF_8);
            MemorySegment text = (MemorySegment) Native.PROGRAM_INTERFACE.invokeExact(handle.address(), m, error);
            String message = Native.takeString(error.get(ADDRESS, 0), true);
            if (text.address() == 0) {
                throw new QuartzException(message == null ? "the program has no package interface" : message);
            }
            return Native.takeString(text, true);
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
