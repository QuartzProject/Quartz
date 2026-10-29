package org.quartzlang.jvm;

import static java.lang.foreign.ValueLayout.JAVA_BYTE;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.ref.Cleaner;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;

/** An isolated instance of a program with its own memory, fuel and state, used by one thread at a time. */
public final class Sandbox implements AutoCloseable {
    private volatile Program program;
    private final NativeHandle handle;
    private final MemorySegment address;
    private final Cleaner.Cleanable cleanable;
    volatile Confinement confinement;
    private volatile boolean closed;
    private boolean active;
    // Only held so the host functions native code may still call stay reachable.
    private final List<HostDispatch.Binding> bindings = new ArrayList<>();
    private final Map<Integer, Type> taskResults = new HashMap<>();
    private final List<Sandbox> providers = new ArrayList<>();
    private final HostCall hostCall = new HostCall(this);
    private final Arena scratch = Arena.ofAuto();
    private final MemorySegment result = scratch.allocate(JAVA_LONG);
    private final MemorySegment outcome = scratch.allocate(Native.TASK_OUTCOME);
    private final MemorySegment length = scratch.allocate(JAVA_LONG);
    private MemorySegment wide;
    private MemorySegment argv = scratch.allocate(JAVA_LONG, 8);

    public Sandbox(Program program) {
        this(program, Limits.DEFAULT);
    }

    public Sandbox(Program program, Limits limits) {
        this.program = Objects.requireNonNull(program, "program");
        NativeHandle programHandle = program.handle();
        programHandle.retain();
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment l = arena.allocate(Native.LIMITS);
            l.set(JAVA_LONG, 0, limits.memoryBytes());
            l.set(JAVA_LONG, 8, limits.fuelPerCall());
            l.set(JAVA_INT, 16, limits.maxDepth());
            l.set(JAVA_INT, 20, limits.tier().ordinal());
            MemorySegment sb = (MemorySegment) Native.SANDBOX_NEW.invokeExact(programHandle.address(), l);
            if (sb.address() == 0) {
                throw new QuartzException("cannot create a sandbox");
            }
            address = sb;
            handle = new NativeHandle(sb, Sandbox::free);
            handle.dependOn(programHandle);
        } catch (Throwable t) {
            throw Native.rethrow(t);
        } finally {
            programHandle.release();
        }
        confinement = new Confinement(this);
        cleanable = Cleaning.CLEANER.register(this, handle::release);
    }

    public Program program() {
        return program;
    }

    public boolean isClosed() {
        return closed;
    }

    /** Binds the host import {@code importName} as the .qzi spells it ({@code greet}, {@code Player.level}). */
    public void bind(String importName, HostFunction function) {
        Objects.requireNonNull(function, "function");
        Import declared = program.findImport(importName).orElseThrow(
                () -> new IllegalArgumentException("the program imports nothing named " + importName));
        String name = declared.name();
        HostDispatch.Binding binding = new HostDispatch.Binding(this, function, name, declared.signature());
        Confinement c = begin();
        try (Arena arena = Arena.ofConfined()) {
            int index = HostDispatch.register(binding);
            boolean bound = (boolean) Native.SANDBOX_BIND_DIRECT.invokeExact(address,
                    arena.allocateFrom(name, StandardCharsets.UTF_8), HostDispatch.STUB,
                    MemorySegment.ofAddress(index));
            if (!bound) {
                HostDispatch.unregister(index);
                throw new IllegalArgumentException("the program imports nothing named " + importName);
            }
            bindings.add(binding);
            handle.afterFree(() -> HostDispatch.unregister(index));
        } catch (Throwable t) {
            throw Native.rethrow(t);
        } finally {
            end(c);
        }
    }

    /** Checks every import is bound, creates the states and runs their initializers. */
    public void start() {
        Confinement c = begin();
        try {
            c.generation++;
            check((int) Native.SANDBOX_START.invokeExact(address), c);
        } catch (Throwable t) {
            throw Native.rethrow(t);
        } finally {
            end(c);
        }
    }

    /** Calls the exported function {@code name}; see {@link #call(Export, Object...)}. */
    public Value call(String name, Object... args) {
        return call(program.export(name), args);
    }

    /**
     * Calls an exported function; {@code state} parameters are supplied by the sandbox and traps throw. Arguments are
     * boxed scalars, String, byte[], List or array (lists and tuples), Map (maps, or a struct by field name), a record
     * or Kotlin data class (a struct by field name, a tuple by position), an enum constant, {@link Variant},
     * {@link Result}, a sealed subclass named like a variant, null for an empty nullable, or a {@link Value}.
     */
    public Value call(Export function, Object... args) {
        requireOwn(function);
        Confinement c = begin();
        try {
            long count = slots(function, args);
            Type type = function.signature().result();
            c.generation++;
            c.hostFailure = null;
            if (type.slots() > 1) {
                MemorySegment out = wideResult(type.slots());
                check((int) Native.SANDBOX_CALL_WIDE.invokeExact(address, function.index(), argv, count, out,
                        (long) type.slots()), c);
                long[] wide = out.asSlice(0, (long) type.slots() * Long.BYTES).toArray(JAVA_LONG);
                return new Value(wide[0], this, c.generation, type, wide);
            }
            check((int) Native.SANDBOX_CALL.invokeExact(address, function.index(), argv, count, result), c);
            return new Value(result.get(JAVA_LONG, 0), this, c.generation, type);
        } catch (Throwable t) {
            throw Native.rethrow(t);
        } finally {
            end(c);
        }
    }

    /** Starts a task (a {@code suspend fn}); it runs until it finishes, waits or spends its fuel slice. */
    public TaskOutcome spawn(Export function, Object... args) {
        requireOwn(function);
        Confinement c = begin();
        try {
            long count = slots(function, args);
            c.generation++;
            c.hostFailure = null;
            check((int) Native.SANDBOX_SPAWN.invokeExact(address, function.index(), argv, count, outcome), c);
            taskResults.put(outcome.get(JAVA_INT, 4), function.signature().result());
            return outcome(c);
        } catch (Throwable t) {
            throw Native.rethrow(t);
        } finally {
            end(c);
        }
    }

    public TaskOutcome spawn(String name, Object... args) {
        return spawn(program.export(name), args);
    }

    /** Resumes a task with what its {@code wait} produces, checked against the awaited type. */
    public TaskOutcome resume(int task, Object value) {
        Confinement c = begin();
        try {
            int waitType = (int) Native.SANDBOX_WAIT_TYPE.invokeExact(address, task);
            if (waitType == 0) {
                throw new IllegalArgumentException("task " + task + " is not waiting for a value");
            }
            Type awaited = program.typeOf(waitType);
            if (awaited.slots() == 1 && !Slots.isComposite(awaited.kind())) {
                long slot = value == null ? 0 : Slots.encode(awaited, value, this, makers);
                c.generation++;
                c.hostFailure = null;
                check((int) Native.SANDBOX_RESUME.invokeExact(address, task, slot, outcome), c);
                return outcome(c);
            }
            long[] slots = new long[Math.max(1, awaited.slots())];
            Slots.encode(awaited, value, slots, 0, this, makers);
            MemorySegment cell = wideResult(slots.length);
            MemorySegment.copy(slots, 0, cell, JAVA_LONG, 0, slots.length);
            c.generation++;
            c.hostFailure = null;
            check((int) Native.SANDBOX_RESUME_WIDE.invokeExact(address, task, cell, (long) awaited.slots(), outcome),
                    c);
            return outcome(c);
        } catch (Throwable t) {
            throw Native.rethrow(t);
        } finally {
            end(c);
        }
    }

    public void cancel(int task) {
        Confinement c = begin();
        try {
            c.generation++;
            taskResults.remove(task);
            Native.SANDBOX_CANCEL.invokeExact(address, task);
        } catch (Throwable t) {
            throw Native.rethrow(t);
        } finally {
            end(c);
        }
    }

    /**
     * {@code text} as a {@code str} of this sandbox, each ill-formed UTF-8 sequence becoming U+FFFD; it can be passed
     * to a call and read until the next one. {@link LossyText} does the same inside lists, structs and results.
     */
    public Value strLossy(byte[] text) {
        Objects.requireNonNull(text, "text");
        Confinement c = enter();
        try {
            return new Value(newLossyString(text), this, c.generation, new Type(Kind.STR, 1));
        } catch (Throwable t) {
            throw Native.rethrow(t);
        } finally {
            c.exit();
        }
    }

    /** Every state as JSON, with field names and versions, for persistence or a reload. */
    public String exportState() {
        Confinement c = begin();
        try {
            MemorySegment json = (MemorySegment) Native.SANDBOX_EXPORT_STATE.invokeExact(address);
            if (json.address() == 0) {
                throw new TrapException(trapCode(), error());
            }
            return Native.takeString(json, true);
        } catch (Throwable t) {
            throw Native.rethrow(t);
        } finally {
            end(c);
        }
    }

    /** Loads states written by {@link #exportState()} of this or an older version of the program. */
    public void importState(String json) {
        Confinement c = begin();
        try (Arena arena = Arena.ofConfined()) {
            byte[] utf8 = json.getBytes(StandardCharsets.UTF_8);
            MemorySegment text = arena.allocate(Math.max(1, utf8.length));
            MemorySegment.copy(utf8, 0, text, JAVA_BYTE, 0, utf8.length);
            c.generation++;
            check((int) Native.SANDBOX_IMPORT_STATE.invokeExact(address, text, (long) utf8.length), c);
        } catch (Throwable t) {
            throw Native.rethrow(t);
        } finally {
            end(c);
        }
    }

    /** Binds this sandbox's imports {@code pkg.fn} to {@code provider}'s exports, run on the provider's budget. */
    public void link(String pkg, Sandbox provider) {
        if (provider == this) {
            throw new IllegalArgumentException("a sandbox cannot link to itself");
        }
        Confinement mine = begin();
        Confinement theirs = null;
        try (Arena arena = Arena.ofConfined()) {
            if (mine.nested()) {
                throw new IllegalStateException("link sandboxes outside any call");
            }
            theirs = Confinement.enter(provider);
            if (provider.closed || provider.active || (theirs != mine && theirs.nested())) {
                throw new IllegalStateException("the provider is closed or running a call");
            }
            int status = (int) Native.SANDBOX_LINK.invokeExact(address, arena.allocateFrom(pkg,
                    StandardCharsets.UTF_8), provider.address);
            if (status != Native.OK) {
                throw new QuartzException(error());
            }
            handle.dependOn(provider.handle);
            providers.add(provider);
            if (theirs != mine) {
                mine.absorb(theirs);
                theirs = null;
            }
        } catch (Throwable t) {
            throw Native.rethrow(t);
        } finally {
            if (theirs != null) {
                theirs.exit();
            }
            end(mine);
        }
    }

    /** Replaces the program in place, keeping bindings, limits and migrated state; on failure nothing changes. */
    public void reload(Program next) {
        Objects.requireNonNull(next, "next");
        Confinement c = begin();
        NativeHandle nextHandle = next.handle();
        nextHandle.retain();
        try {
            for (HostDispatch.Binding b : bindings) {
                Import declared = next.findImport(b.name).orElse(null);
                if (declared == null) {
                    continue;
                }
                b.signature = declared.signature();
            }
            c.generation++;
            c.hostFailure = null;
            try {
                check((int) Native.SANDBOX_RELOAD.invokeExact(address, nextHandle.address()), c);
            } catch (Throwable t) {
                for (HostDispatch.Binding b : bindings) {
                    program.findImport(b.name).ifPresent(i -> b.signature = i.signature());
                }
                throw t;
            }
            handle.dependOn(nextHandle);
            program = next;
            taskResults.clear();
        } catch (Throwable t) {
            throw Native.rethrow(t);
        } finally {
            nextHandle.release();
            end(c);
        }
    }

    /** The {@code @persist(per = Handle)} states of {@code handle} as JSON, to save when it leaves. */
    public String exportHandle(long handleId) {
        Confinement c = begin();
        try {
            MemorySegment json = (MemorySegment) Native.SANDBOX_EXPORT_HANDLE.invokeExact(address, handleId);
            if (json.address() == 0) {
                throw new TrapException(trapCode(), error());
            }
            return Native.takeString(json, true);
        } catch (Throwable t) {
            throw Native.rethrow(t);
        } finally {
            end(c);
        }
    }

    /** Loads the per-handle states {@link #exportHandle(long)} wrote. */
    public void importHandle(long handleId, String json) {
        Confinement c = begin();
        try (Arena arena = Arena.ofConfined()) {
            byte[] utf8 = json.getBytes(StandardCharsets.UTF_8);
            MemorySegment text = arena.allocate(Math.max(1, utf8.length));
            MemorySegment.copy(utf8, 0, text, JAVA_BYTE, 0, utf8.length);
            c.generation++;
            check((int) Native.SANDBOX_IMPORT_HANDLE.invokeExact(address, handleId, text, (long) utf8.length), c);
        } catch (Throwable t) {
            throw Native.rethrow(t);
        } finally {
            end(c);
        }
    }

    /** Frees the per-handle states of a handle that left; the next call with it starts from the defaults. */
    public void dropHandle(long handleId) {
        Confinement c = begin();
        try {
            Native.SANDBOX_DROP_HANDLE.invokeExact(address, handleId);
        } catch (Throwable t) {
            throw Native.rethrow(t);
        } finally {
            end(c);
        }
    }

    /** Tasks spawned and not yet finished or cancelled. */
    public long liveTasks() {
        Confinement c = enter();
        try {
            return (long) Native.SANDBOX_LIVE_TASKS.invokeExact(address);
        } catch (Throwable t) {
            throw Native.rethrow(t);
        } finally {
            c.exit();
        }
    }

    public long fuelPerCall() {
        Confinement c = enter();
        try {
            return (long) Native.SANDBOX_FUEL_PER_CALL.invokeExact(address);
        } catch (Throwable t) {
            throw Native.rethrow(t);
        } finally {
            c.exit();
        }
    }

    /** Changes the fuel each call and each task slice gets. */
    public void setFuelPerCall(long fuel) {
        Confinement c = enter();
        try {
            Native.SANDBOX_SET_FUEL.invokeExact(address, fuel);
        } catch (Throwable t) {
            throw Native.rethrow(t);
        } finally {
            c.exit();
        }
    }

    /** Fuel left after the last call. */
    public long fuelLeft() {
        Confinement c = enter();
        try {
            return (long) Native.SANDBOX_FUEL_LEFT.invokeExact(address);
        } catch (Throwable t) {
            throw Native.rethrow(t);
        } finally {
            c.exit();
        }
    }

    public long memoryUsed() {
        Confinement c = enter();
        try {
            return (long) Native.SANDBOX_MEMORY_USED.invokeExact(address);
        } catch (Throwable t) {
            throw Native.rethrow(t);
        } finally {
            c.exit();
        }
    }

    /** Frees the native sandbox, or hands it to the clients still linked to it. Idempotent. */
    @Override
    public void close() {
        if (closed) {
            return;
        }
        Confinement c = Confinement.enter(this);
        try {
            if (active) {
                throw new IllegalStateException("a sandbox cannot be closed from inside its own call");
            }
            if (!closed) {
                closed = true;
                cleanable.clean();
            }
        } finally {
            c.exit();
        }
    }

    boolean isCurrent(long generation) {
        return !closed && confinement.generation == generation;
    }

    byte[] readString(long bits, long generation) {
        Confinement c = enter();
        try {
            requireCurrent(c, generation);
            return readSlotString(bits);
        } finally {
            c.exit();
        }
    }

    int listLength(long bits, long generation) {
        Confinement c = enter();
        try {
            requireCurrent(c, generation);
            return Math.toIntExact((long) Native.LIST_LENGTH.invokeExact(bits));
        } catch (Throwable t) {
            throw Native.rethrow(t);
        } finally {
            c.exit();
        }
    }

    /** Reads str, list and map slots, checking that no later call freed them; a negative generation skips it. */
    Slots.Readers readers(long generation) {
        return new Slots.Readers() {
            @Override
            public byte[] string(long bits) {
                return generation < 0 ? readSlotString(bits) : readString(bits, generation);
            }

            @Override
            public List<Object> list(Type type, long bits) {
                if (generation < 0) {
                    return Heap.readList(type, bits, this);
                }
                Confinement c = enter();
                try {
                    requireCurrent(c, generation);
                    return Heap.readList(type, bits, this);
                } finally {
                    c.exit();
                }
            }

            @Override
            public Map<Object, Object> map(Type type, long bits) {
                if (generation < 0) {
                    return Heap.readMap(address, type, bits, this);
                }
                Confinement c = enter();
                try {
                    requireCurrent(c, generation);
                    return Heap.readMap(address, type, bits, this);
                } finally {
                    c.exit();
                }
            }
        };
    }

    /** A list of {@code type} filled with {@code elements}, as an argument or, in a host function, its result. */
    long newList(Type type, List<?> elements, Slots.Makers makers) throws Throwable {
        return Heap.newList(address, this, type, elements, makers);
    }

    /** A map of {@code type} with {@code entries} in their iteration order. */
    long newMap(Type type, Map<?, ?> entries, Slots.Makers makers) throws Throwable {
        return Heap.newMap(address, this, type, entries, makers);
    }

    byte[] readSlotString(long bits) {
        try {
            long text = (long) Native.STR.invokeExact(bits, length);
            byte[] out = new byte[Math.toIntExact(length.get(JAVA_LONG, 0))];
            MemorySegment.copy(Native.ALL, JAVA_BYTE, text, out, 0, out.length);
            return out;
        } catch (Throwable t) {
            throw Native.rethrow(t);
        }
    }

    boolean runHost(HostDispatch.Binding binding, long call, long args, long res) {
        HostCall hc = hostCall.active() ? new HostCall(this) : hostCall;
        hc.begin(binding.signature, call, args, res);
        try {
            hc.setResult(binding.function.call(hc));
            return true;
        } catch (Throwable t) {
            confinement.hostFailure = t;
            HostDispatch.fail(call, describe(t));
            return false;
        } finally {
            hc.end();
        }
    }

    private final Slots.Makers makers = new Slots.Makers() {
        @Override
        public long string(byte[] utf8) throws Throwable {
            return newString(utf8);
        }

        @Override
        public long bytes(byte[] data) throws Throwable {
            long slot = (long) Native.SANDBOX_BYTES.invokeExact(address, MemorySegment.ofArray(data),
                    (long) data.length);
            if (slot == 0) {
                throw new TrapException(TrapCode.OUT_OF_MEMORY, "out of memory passing Bytes");
            }
            return slot;
        }

        @Override
        public long stringLossy(byte[] text) throws Throwable {
            return newLossyString(text);
        }

        @Override
        public long list(Type type, List<?> elements) throws Throwable {
            return newList(type, elements, this);
        }

        @Override
        public long map(Type type, Map<?, ?> entries) throws Throwable {
            return newMap(type, entries, this);
        }
    };

    private MemorySegment wideResult(int slots) {
        if (wide == null || wide.byteSize() < (long) slots * Long.BYTES) {
            wide = scratch.allocate(JAVA_LONG, Math.max(slots, 4));
        }
        return wide;
    }

    private long newLossyString(byte[] text) throws Throwable {
        long slot = (long) Native.SANDBOX_STR_LOSSY.invokeExact(address, MemorySegment.ofArray(text),
                (long) text.length);
        if (slot == 0) {
            throw new TrapException(TrapCode.OUT_OF_MEMORY, "out of memory passing a string");
        }
        return slot;
    }

    private long newString(byte[] utf8) throws Throwable {
        long slot = (long) Native.SANDBOX_STR.invokeExact(address, MemorySegment.ofArray(utf8), (long) utf8.length);
        if (slot == 0) {
            throw new TrapException(TrapCode.OUT_OF_MEMORY, "out of memory passing a string");
        }
        return slot;
    }

    /** Fills {@code argv} with the arguments' slots and returns how many there are. */
    private long slots(Export function, Object[] args) throws Throwable {
        Signature signature = function.signature();
        if (args.length != signature.arity()) {
            throw new IllegalArgumentException("expected " + signature.arity() + " arguments " + signature.params()
                    + ", got " + args.length);
        }
        int count = function.argSlots();
        if (count > argv.byteSize() / Long.BYTES) {
            argv = scratch.allocate(JAVA_LONG, Math.max(count, argv.byteSize() / Long.BYTES * 2));
        }
        if (function.simple()) {
            for (int i = 0; i < args.length; ++i) {
                argv.setAtIndex(JAVA_LONG, i, Slots.encode(signature.params().get(i), args[i], this, makers));
            }
            return count;
        }
        long[] wideArgs = new long[count];
        for (int i = 0; i < args.length; ++i) {
            Slots.encode(signature.params().get(i), args[i], wideArgs, function.offset(i), this, makers);
        }
        MemorySegment.copy(wideArgs, 0, argv, JAVA_LONG, 0, count);
        return count;
    }

    private TaskOutcome outcome(Confinement c) throws Throwable {
        TaskOutcome.Kind[] kinds = TaskOutcome.Kind.values();
        int code = outcome.get(JAVA_INT, 0);
        TaskOutcome.Kind kind = code >= 0 && code < kinds.length ? kinds[code] : TaskOutcome.Kind.YIELD;
        int task = outcome.get(JAVA_INT, 4);
        Type type = new Type(Kind.HANDLE, 1);
        long bits = outcome.get(JAVA_LONG, 8);
        if (kind == TaskOutcome.Kind.DONE) {
            Type declared = taskResults.remove(task);
            type = declared == null ? new Type(Kind.OTHER, 1) : declared;
            if (type.slots() > 1) {
                MemorySegment out = wideResult(type.slots());
                check((int) Native.SANDBOX_TASK_RESULT.invokeExact(address, task, out, (long) type.slots()), c);
                long[] wide = out.asSlice(0, (long) type.slots() * Long.BYTES).toArray(JAVA_LONG);
                return new TaskOutcome(kind, task, new Value(wide[0], this, c.generation, type, wide));
            }
        }
        return new TaskOutcome(kind, task, new Value(bits, this, c.generation, type));
    }

    private void requireOwn(Export function) {
        if (function.program() != program) {
            throw new IllegalArgumentException(function + " belongs to another program");
        }
    }

    private void check(int status, Confinement c) {
        if (status == Native.OK) {
            return;
        }
        Throwable cause = c.hostFailure;
        c.hostFailure = null;
        if (status == Native.TRAP) {
            throw new TrapException(trapCode(), error(), cause);
        }
        throw new QuartzException(error());
    }

    private TrapCode trapCode() {
        try {
            return TrapCode.of((int) Native.SANDBOX_TRAP_CODE.invokeExact(address));
        } catch (Throwable t) {
            throw Native.rethrow(t);
        }
    }

    private static void requireCurrent(Confinement c, long generation) {
        if (c.generation != generation) {
            throw new IllegalStateException("a later call freed this result; copy it out before calling again");
        }
    }

    private String error() {
        try {
            String message = Native.takeString((MemorySegment) Native.SANDBOX_ERROR.invokeExact(address), false);
            return message == null || message.isEmpty() ? "unknown error" : message;
        } catch (Throwable t) {
            throw Native.rethrow(t);
        }
    }

    private static String describe(Throwable t) {
        if (t instanceof QuartzException && t.getMessage() != null) {
            return t.getMessage();
        }
        return t.getMessage() == null ? t.getClass().getName() : t.getClass().getSimpleName() + ": " + t.getMessage();
    }

    private Confinement enter() {
        Confinement c = Confinement.enter(this);
        if (closed) {
            c.exit();
            throw new IllegalStateException("sandbox is closed");
        }
        return c;
    }

    private Confinement begin() {
        Confinement c = enter();
        if (active) {
            c.exit();
            throw new IllegalStateException("sandbox is already running a call; it cannot be re-entered");
        }
        active = true;
        return c;
    }

    private void end(Confinement c) {
        active = false;
        c.exit();
    }

    private static void free(MemorySegment address) {
        try {
            Native.SANDBOX_FREE.invokeExact(address);
        } catch (Throwable t) {
            throw Native.rethrow(t);
        }
    }
}
