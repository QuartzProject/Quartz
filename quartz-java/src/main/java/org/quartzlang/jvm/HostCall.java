package org.quartzlang.jvm;

import static java.lang.foreign.ValueLayout.JAVA_LONG;

import java.lang.foreign.MemorySegment;
import java.nio.charset.StandardCharsets;

/** A running host function's arguments, read as the declared types, and its fuel; valid only while it runs. */
public final class HostCall {
    /** The C interface does not give the argument count; reads past this bound fail instead of wandering. */
    private static final long MAX_ARGS = 255;

    private final Sandbox sandbox;
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

    public long longArg(int index) {
        return rawArg(index);
    }

    public int intArg(int index) {
        return (int) rawArg(index);
    }

    public double doubleArg(int index) {
        return Double.longBitsToDouble(rawArg(index));
    }

    public float floatArg(int index) {
        return Float.intBitsToFloat((int) rawArg(index));
    }

    public boolean boolArg(int index) {
        return rawArg(index) != 0;
    }

    /** Only for a {@code str} parameter. */
    public String stringArg(int index) {
        return new String(bytesArg(index), StandardCharsets.UTF_8);
    }

    /** Only for a {@code str} or {@code Bytes} parameter. */
    public byte[] bytesArg(int index) {
        return sandbox.readSlotString(rawArg(index));
    }

    /** The raw 64 bits of argument {@code index}; the caller must stay within the declared parameters. */
    public long rawArg(int index) {
        if (index < 0 || index >= MAX_ARGS) {
            throw new IndexOutOfBoundsException(index);
        }
        return Native.ALL.get(JAVA_LONG, args() + (long) index * Long.BYTES);
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

    void begin(long nativeCall) {
        call = nativeCall;
        args = 0;
        active = true;
    }

    void end() {
        active = false;
        call = 0;
        args = 0;
    }

    void setResult(Object value) throws Throwable {
        if (value == null) {
            return;
        }
        long bits;
        if (value instanceof String s) {
            bits = string(s.getBytes(StandardCharsets.UTF_8));
        } else if (value instanceof byte[] b) {
            bits = string(b);
        } else {
            bits = sandbox.scalarSlot(value);
        }
        Native.CALL_SET_RESULT.invokeExact(call, bits);
    }

    private long string(byte[] utf8) throws Throwable {
        long slot = (long) Native.CALL_STR.invokeExact(call, MemorySegment.ofArray(utf8), (long) utf8.length);
        if (slot == 0) {
            throw new TrapException("out of memory returning a string");
        }
        return slot;
    }

    private long args() {
        check();
        if (args == 0) {
            try {
                args = (long) Native.CALL_ARGS.invokeExact(call);
            } catch (Throwable t) {
                throw Native.rethrow(t);
            }
        }
        return args;
    }

    private void check() {
        if (!active) {
            throw new IllegalStateException("the host call is over");
        }
    }
}
