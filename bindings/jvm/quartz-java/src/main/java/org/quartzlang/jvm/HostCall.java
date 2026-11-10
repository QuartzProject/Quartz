package org.quartzlang.jvm;

import static java.lang.foreign.ValueLayout.JAVA_LONG;

import java.lang.foreign.MemorySegment;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Map;

/** A running host function's arguments, checked against the import's types, and its fuel; valid while it runs. */
public final class HostCall {
    private final Sandbox sandbox;
    private Signature signature;
    private long call;
    private long args;
    private long result;
    private boolean active;

    HostCall(Sandbox sandbox) {
        this.sandbox = sandbox;
    }

    /** The sandbox that owns the binding; calling into it from here throws, it is already running. */
    public Sandbox sandbox() {
        return sandbox;
    }

    /** The import's parameter and result types. */
    public Signature signature() {
        check();
        return signature;
    }

    public int arity() {
        return signature().arity();
    }

    public long longArg(int index) {
        Kind k = kind(index);
        if (k == Kind.I64) {
            return rawArg(index);
        }
        if (!(k.isInteger() || k == Kind.HANDLE || k == Kind.CHAR || k == Kind.BOOL || k == Kind.ENUM)) {
            throw wrong(index, "longArg");
        }
        return Slots.widen(k, rawArg(index));
    }

    public int intArg(int index) {
        return (int) longArg(index);
    }

    public double doubleArg(int index) {
        return switch (kind(index)) {
            case F64 -> Double.longBitsToDouble(rawArg(index));
            case F32 -> Float.intBitsToFloat((int) rawArg(index));
            default -> throw wrong(index, "doubleArg");
        };
    }

    public float floatArg(int index) {
        return (float) doubleArg(index);
    }

    public boolean boolArg(int index) {
        if (kind(index) != Kind.BOOL) {
            throw wrong(index, "boolArg");
        }
        return rawArg(index) != 0;
    }

    public int codePointArg(int index) {
        if (kind(index) != Kind.CHAR) {
            throw wrong(index, "codePointArg");
        }
        return (int) rawArg(index);
    }

    public String stringArg(int index) {
        return new String(bytesArg(index), StandardCharsets.UTF_8);
    }

    public byte[] bytesArg(int index) {
        Kind k = kind(index);
        if (k != Kind.STR && k != Kind.BYTES) {
            throw wrong(index, "bytesArg");
        }
        return sandbox.readSlotString(rawArg(index));
    }

    /** Argument {@code index} boxed by its type, as {@link Value#get()} does. */
    public Object arg(int index) {
        kind(index);
        Type type = signature.params().get(index);
        if (type.slots() == 1 && !Slots.isComposite(type.kind())) {
            return Slots.box(type, rawArg(index), sandbox.readers());
        }
        long[] slots = new long[Math.max(1, type.slots())];
        MemorySegment.copy(Native.ALL, JAVA_LONG, args + (long) signature.offset(index) * Long.BYTES, slots, 0,
                type.slots());
        return Slots.decode(type, slots, 0, sandbox.readers());
    }

    /** Argument {@code index} converted to {@code target}, as {@link Value#to(Class)} does. */
    public <T> T arg(int index, Class<T> target) {
        Object o = Mapper.convert(arg(index), target);
        return o == null ? null : boxed(target).cast(o);
    }

    /** Argument {@code index} converted to a generic type: {@code call.arg(0, new TypeRef<List<Point>>() {})}. */
    @SuppressWarnings("unchecked")
    public <T> T arg(int index, TypeRef<T> target) {
        return (T) Mapper.convert(arg(index), target.type());
    }

    /** Argument {@code index} converted to a generic type given as a {@link java.lang.reflect.Type}. */
    public Object arg(int index, java.lang.reflect.Type target) {
        return Mapper.convert(arg(index), target);
    }

    /** Argument {@code index} converted to {@code target}, every handle inside mapped by {@code handles}. */
    public Object arg(int index, java.lang.reflect.Type target, HandleMapper handles) {
        return TypedValues.to(arg(index), signature.params().get(index), target, handles);
    }

    /** A list argument with its elements boxed by the element type. */
    public List<Object> listArg(int index) {
        if (kind(index) != Kind.LIST) {
            throw wrong(index, "listArg");
        }
        @SuppressWarnings("unchecked")
        List<Object> list = (List<Object>) Slots.box(signature.params().get(index), rawArg(index),
                sandbox.readers());
        return list;
    }

    /** A map argument in insertion order. */
    public Map<Object, Object> mapArg(int index) {
        if (kind(index) != Kind.MAP) {
            throw wrong(index, "mapArg");
        }
        @SuppressWarnings("unchecked")
        Map<Object, Object> map = (Map<Object, Object>) Slots.box(signature.params().get(index), rawArg(index),
                sandbox.readers());
        return map;
    }

    /** The first raw slot of parameter {@code index}. */
    public long rawArg(int index) {
        Signature s = signature();
        if (index < 0 || index >= s.arity()) {
            throw new IndexOutOfBoundsException("argument " + index + " of " + s.arity());
        }
        return Native.ALL.get(JAVA_LONG, args + (long) s.offset(index) * Long.BYTES);
    }

    /**
     * {@code text} as a {@code str} result, each ill-formed UTF-8 sequence becoming U+FFFD: return it, or put it in
     * a list, struct or variant the function returns.
     */
    public LossyText stringLossy(byte[] text) {
        return new LossyText(text);
    }

    /** Charges fuel for native work; false when the budget is gone, and the call then traps whatever is returned. */
    public boolean charge(long units) {
        check();
        try {
            return (boolean) Native.CALL_CHARGE.invokeExact(call, units);
        } catch (Throwable t) {
            throw Native.rethrow(t);
        }
    }

    boolean active() {
        return active;
    }

    void begin(Signature sig, long nativeCall, long nativeArgs, long nativeResult) {
        signature = sig;
        call = nativeCall;
        args = nativeArgs;
        result = nativeResult;
        active = true;
    }

    void end() {
        active = false;
        call = 0;
        args = 0;
    }

    void setResult(Object value) throws Throwable {
        Type type = signature.result();
        if (type.kind() == Kind.VOID) {
            return;
        }
        if (type.slots() == 1 && !Slots.isComposite(type.kind())) {
            Native.ALL.set(JAVA_LONG, result, Slots.encode(type, value, sandbox, makers));
            return;
        }
        long[] slots = new long[Math.max(1, type.slots())];
        Slots.encode(type, value, slots, 0, sandbox, makers);
        MemorySegment.copy(slots, 0, Native.ALL, JAVA_LONG, result, type.slots());
    }

    private final Slots.Makers makers = new Slots.Makers() {
        @Override
        public long string(byte[] utf8) throws Throwable {
            return HostCall.this.string(utf8);
        }

        @Override
        public long bytes(byte[] data) throws Throwable {
            long slot = (long) Native.CALL_BYTES.invokeExact(call, MemorySegment.ofArray(data), (long) data.length);
            if (slot == 0) {
                throw new TrapException(TrapCode.OUT_OF_MEMORY, "out of memory returning Bytes");
            }
            return slot;
        }

        @Override
        public long stringLossy(byte[] text) throws Throwable {
            long slot = (long) Native.CALL_STR_LOSSY.invokeExact(call, MemorySegment.ofArray(text),
                    (long) text.length);
            if (slot == 0) {
                throw new TrapException(TrapCode.OUT_OF_MEMORY, "out of memory returning a string");
            }
            return slot;
        }

        @Override
        public long list(Type type, long[] slots, int count) throws Throwable {
            return sandbox.newList(type, slots, count);
        }

        @Override
        public long map(Type type, long[] keys, long[] values, int count) throws Throwable {
            return sandbox.newMap(type, keys, values, count);
        }
    };

    private long string(byte[] utf8) throws Throwable {
        long slot = (long) Native.CALL_STR.invokeExact(call, MemorySegment.ofArray(utf8), (long) utf8.length);
        if (slot == 0) {
            throw new TrapException(TrapCode.OUT_OF_MEMORY, "out of memory returning a string");
        }
        return slot;
    }

    @SuppressWarnings("unchecked")
    private static <T> Class<T> boxed(Class<T> c) {
        return c.isPrimitive() ? (Class<T>) java.lang.invoke.MethodType.methodType(c).wrap().returnType() : c;
    }

    private Kind kind(int index) {
        Signature s = signature();
        if (index < 0 || index >= s.arity()) {
            throw new IndexOutOfBoundsException("argument " + index + " of " + s.arity());
        }
        return s.params().get(index).kind();
    }

    private IllegalStateException wrong(int index, String what) {
        return new IllegalStateException("argument " + index + " is " + signature.params().get(index) + ", " + what
                + " does not apply");
    }

    private void check() {
        if (!active) {
            throw new IllegalStateException("the host call is over");
        }
    }
}
