package org.quartzlang.jvm;

import static java.lang.foreign.ValueLayout.JAVA_BOOLEAN;
import static java.lang.foreign.ValueLayout.JAVA_LONG;

import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.MemorySegment;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.ref.WeakReference;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.Arrays;

/** One upcall stub serves every host function; {@code user} indexes weak bindings so sandboxes stay collectable. */
final class HostDispatch {
    record Binding(Sandbox sandbox, HostFunction function) {}

    static final MemorySegment STUB;

    private static volatile Object[] table = new Object[64];
    private static final ArrayDeque<Integer> FREE = new ArrayDeque<>();
    private static int next = 1;

    static {
        try {
            var target = MethodHandles.lookup().findStatic(HostDispatch.class, "dispatch",
                    MethodType.methodType(boolean.class, long.class, long.class));
            STUB = Native.linker().upcallStub(target, FunctionDescriptor.of(JAVA_BOOLEAN, JAVA_LONG, JAVA_LONG),
                    Arena.global());
        } catch (ReflectiveOperationException e) {
            throw new ExceptionInInitializerError(e);
        }
    }

    private HostDispatch() {}

    static synchronized int register(Binding binding) {
        int index = FREE.isEmpty() ? next++ : FREE.pop();
        Object[] t = table;
        if (index >= t.length) {
            t = Arrays.copyOf(t, t.length * 2);
        }
        t[index] = new WeakReference<>(binding);
        table = t;
        return index;
    }

    /** Only once no native code can call {@code index} again. */
    static synchronized void unregister(int index) {
        table[index] = null;
        FREE.push(index);
    }

    @SuppressWarnings("unused")
    private static boolean dispatch(long call, long user) {
        try {
            Binding binding = lookup(user);
            if (binding == null) {
                fail(call, "host function was released");
                return false;
            }
            return binding.sandbox().runHost(binding.function(), call);
        } catch (Throwable t) {
            fail(call, "host error");
            return false;
        }
    }

    @SuppressWarnings("unchecked")
    private static Binding lookup(long index) {
        Object[] t = table;
        if (index <= 0 || index >= t.length || t[(int) index] == null) {
            return null;
        }
        return ((WeakReference<Binding>) t[(int) index]).get();
    }

    /** Never throws: an exception escaping an upcall would abort the JVM. */
    static void fail(long call, String message) {
        try (Arena arena = Arena.ofConfined()) {
            Native.CALL_FAIL.invokeExact(call, arena.allocateFrom(message, StandardCharsets.UTF_8));
        } catch (Throwable ignored) {
            // the trap keeps its generic message
        }
    }
}
