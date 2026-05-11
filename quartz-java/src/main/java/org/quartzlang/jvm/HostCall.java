package org.quartzlang.jvm;

import static java.lang.foreign.ValueLayout.JAVA_LONG;

import java.lang.foreign.MemorySegment;
import java.nio.charset.StandardCharsets;
import java.util.List;

/** A running host function's arguments, checked against the import's types, and its fuel; valid while it runs. */
public final class HostCall {
    private final Sandbox sandbox;
    private Signature signature;
    private long call;
    private long args;
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

    /** Argument {@code index} boxed by its kind, as {@link Value#get()} does. */
    public Object arg(int index) {
        Kind k = kind(index);
        if (k == Kind.STR || k == Kind.BYTES) {
            byte[] b = bytesArg(index);
            return k == Kind.STR ? new String(b, StandardCharsets.UTF_8) : b;
        }
        return Slots.box(k, rawArg(index), ignored -> {
            throw new IllegalStateException("unreachable");
        });
    }

    /** A list argument, boxed as {@code element} says; that is the caller's word, see {@link Value#asList(Kind)}. */
    public List<Object> listArg(int index, Kind element) {
        if (kind(index) != Kind.LIST) {
            throw wrong(index, "listArg");
        }
        return sandbox.readListSlot(rawArg(index), element);
    }

    /** The first raw slot of parameter {@code index}. */
    public long rawArg(int index) {
        Signature s = signature();
        if (index < 0 || index >= s.arity()) {
            throw new IndexOutOfBoundsException("argument " + index + " of " + s.arity());
        }
        return Native.ALL.get(JAVA_LONG, args + (long) s.offset(index) * Long.BYTES);
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

    void begin(Signature sig, long nativeCall, long nativeArgs) {
        signature = sig;
        call = nativeCall;
        args = nativeArgs;
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
        long bits = Slots.encode(type, value, sandbox, this::string);
        // A direct binding's one-slot result is overwritten by HostCall::result after it returns, so set that.
        Native.CALL_SET_RESULT.invokeExact(call, bits);
    }

    private long string(byte[] utf8) throws Throwable {
        long slot = (long) Native.CALL_STR.invokeExact(call, MemorySegment.ofArray(utf8), (long) utf8.length);
        if (slot == 0) {
            throw new TrapException(TrapCode.OUT_OF_MEMORY, "out of memory returning a string");
        }
        return slot;
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
