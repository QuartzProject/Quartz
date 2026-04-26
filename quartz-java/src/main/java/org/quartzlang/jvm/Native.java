package org.quartzlang.jvm;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_BOOLEAN;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;

import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemoryLayout;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.StructLayout;
import java.lang.foreign.SymbolLookup;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;

/** quartz.h as method handles; a qz_slot or an opaque pointer travels as a long, as every 64-bit ABI passes it. */
final class Native {
    static final int ABI_VERSION = 1;
    static final int OK = 0;
    static final int ERROR = 1;
    static final int TRAP = 2;

    static final ValueLayout.OfLong SIZE_T = JAVA_LONG;
    static final StructLayout SOURCE = MemoryLayout.structLayout(ADDRESS.withName("name"), ADDRESS.withName("text"),
            SIZE_T.withName("length"));
    static final StructLayout COMPILE_OPTIONS = MemoryLayout.structLayout(JAVA_BOOLEAN.withName("emit_binary"),
            JAVA_BOOLEAN.withName("keep_lines"), MemoryLayout.paddingLayout(6), ADDRESS.withName("manifest"));
    static final StructLayout LIMITS = MemoryLayout.structLayout(SIZE_T.withName("memory_bytes"),
            JAVA_LONG.withName("fuel_per_call"), JAVA_INT.withName("max_depth"), JAVA_INT.withName("tier"));
    static final StructLayout TASK_OUTCOME = MemoryLayout.structLayout(JAVA_INT.withName("kind"),
            JAVA_INT.withName("task"), JAVA_LONG.withName("value"));

    /** All of memory, to read through a raw address without wrapping it in a segment first. */
    static final MemorySegment ALL = MemorySegment.NULL.reinterpret(Long.MAX_VALUE);

    private static final Linker LINKER = Linker.nativeLinker();
    private static final SymbolLookup LIB;
    private static final Linker.Option CRITICAL = Linker.Option.critical(false);
    private static final Linker.Option CRITICAL_HEAP = Linker.Option.critical(true);

    static final MethodHandle ABI;
    static final MethodHandle FREE;
    static final MethodHandle COMPILE;
    static final MethodHandle PROGRAM_BINARY;
    static final MethodHandle LOAD;
    static final MethodHandle PROGRAM_FREE;
    static final MethodHandle PROGRAM_FUNCTION;
    static final MethodHandle SANDBOX_NEW;
    static final MethodHandle SANDBOX_FREE;
    static final MethodHandle SANDBOX_BIND;
    static final MethodHandle SANDBOX_START;
    static final MethodHandle SANDBOX_CALL;
    static final MethodHandle SANDBOX_SPAWN;
    static final MethodHandle SANDBOX_RESUME;
    static final MethodHandle SANDBOX_CANCEL;
    static final MethodHandle SANDBOX_ERROR;
    static final MethodHandle SANDBOX_STR;
    static final MethodHandle SANDBOX_SET_FUEL;
    static final MethodHandle SANDBOX_FUEL_LEFT;
    static final MethodHandle SANDBOX_MEMORY_USED;
    static final MethodHandle SANDBOX_EXPORT_STATE;
    static final MethodHandle SANDBOX_IMPORT_STATE;
    static final MethodHandle SANDBOX_LINK;
    static final MethodHandle STR;
    static final MethodHandle CALL_ARGS;
    static final MethodHandle CALL_SET_RESULT;
    static final MethodHandle CALL_STR;
    static final MethodHandle CALL_CHARGE;
    static final MethodHandle CALL_FAIL;

    static {
        if (ADDRESS.byteSize() != 8) {
            throw new UnsatisfiedLinkError("quartz-jvm needs a 64-bit JVM");
        }
        LIB = NativeLoader.load();
        ABI = fn("qz_abi_version", FunctionDescriptor.of(JAVA_INT), CRITICAL);
        FREE = fn("qz_free", FunctionDescriptor.ofVoid(ADDRESS), CRITICAL);
        COMPILE = fn("qz_compile", FunctionDescriptor.of(ADDRESS, ADDRESS, SIZE_T, ADDRESS, ADDRESS));
        PROGRAM_BINARY = fn("qz_program_binary", FunctionDescriptor.of(ADDRESS, ADDRESS, ADDRESS));
        LOAD = fn("qz_load", FunctionDescriptor.of(ADDRESS, ADDRESS, SIZE_T, ADDRESS, SIZE_T, JAVA_BOOLEAN, ADDRESS));
        PROGRAM_FREE = fn("qz_program_free", FunctionDescriptor.ofVoid(ADDRESS));
        PROGRAM_FUNCTION = fn("qz_program_function", FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS), CRITICAL);
        SANDBOX_NEW = fn("qz_sandbox_new", FunctionDescriptor.of(ADDRESS, ADDRESS, ADDRESS));
        SANDBOX_FREE = fn("qz_sandbox_free", FunctionDescriptor.ofVoid(ADDRESS));
        SANDBOX_BIND = fn("qz_sandbox_bind", FunctionDescriptor.of(JAVA_BOOLEAN, ADDRESS, ADDRESS, ADDRESS, ADDRESS));
        SANDBOX_START = fn("qz_sandbox_start", FunctionDescriptor.of(JAVA_INT, ADDRESS));
        SANDBOX_CALL = fn("qz_sandbox_call",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT, ADDRESS, SIZE_T, ADDRESS));
        SANDBOX_SPAWN = fn("qz_sandbox_spawn",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT, ADDRESS, SIZE_T, ADDRESS));
        SANDBOX_RESUME = fn("qz_sandbox_resume",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT, JAVA_LONG, ADDRESS));
        SANDBOX_CANCEL = fn("qz_sandbox_cancel", FunctionDescriptor.ofVoid(ADDRESS, JAVA_INT));
        SANDBOX_ERROR = fn("qz_sandbox_error", FunctionDescriptor.of(ADDRESS, ADDRESS), CRITICAL);
        SANDBOX_STR = fn("qz_sandbox_str", FunctionDescriptor.of(JAVA_LONG, ADDRESS, ADDRESS, SIZE_T), CRITICAL_HEAP);
        SANDBOX_SET_FUEL = fn("qz_sandbox_set_fuel", FunctionDescriptor.ofVoid(ADDRESS, JAVA_LONG), CRITICAL);
        SANDBOX_FUEL_LEFT = fn("qz_sandbox_fuel_left", FunctionDescriptor.of(JAVA_LONG, ADDRESS), CRITICAL);
        SANDBOX_MEMORY_USED = fn("qz_sandbox_memory_used", FunctionDescriptor.of(SIZE_T, ADDRESS), CRITICAL);
        SANDBOX_EXPORT_STATE = fn("qz_sandbox_export_state", FunctionDescriptor.of(ADDRESS, ADDRESS));
        SANDBOX_IMPORT_STATE = fn("qz_sandbox_import_state",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, SIZE_T));
        SANDBOX_LINK = fn("qz_sandbox_link", FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, ADDRESS));
        STR = fn("qz_str", FunctionDescriptor.of(JAVA_LONG, JAVA_LONG, ADDRESS), CRITICAL);
        CALL_ARGS = fn("qz_call_args", FunctionDescriptor.of(JAVA_LONG, JAVA_LONG), CRITICAL);
        CALL_SET_RESULT = fn("qz_call_set_result", FunctionDescriptor.ofVoid(JAVA_LONG, JAVA_LONG), CRITICAL);
        CALL_STR = fn("qz_call_str", FunctionDescriptor.of(JAVA_LONG, JAVA_LONG, ADDRESS, SIZE_T), CRITICAL_HEAP);
        CALL_CHARGE = fn("qz_call_charge", FunctionDescriptor.of(JAVA_BOOLEAN, JAVA_LONG, JAVA_LONG), CRITICAL);
        CALL_FAIL = fn("qz_call_fail", FunctionDescriptor.ofVoid(JAVA_LONG, ADDRESS), CRITICAL);
        int abi = abiVersion();
        if (abi != ABI_VERSION) {
            throw new UnsatisfiedLinkError("libquartz has ABI " + abi + ", quartz-jvm needs " + ABI_VERSION);
        }
    }

    private Native() {}

    private static MethodHandle fn(String name, FunctionDescriptor descriptor, Linker.Option... options) {
        MemorySegment symbol = LIB.find(name).orElseThrow(() -> new UnsatisfiedLinkError("libquartz lacks " + name));
        return LINKER.downcallHandle(symbol, descriptor, options);
    }

    static Linker linker() {
        return LINKER;
    }

    static int abiVersion() {
        try {
            return (int) ABI.invokeExact();
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    static void free(MemorySegment memory) {
        if (memory.address() == 0) {
            return;
        }
        try {
            FREE.invokeExact(memory);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    /** Copies a NUL-terminated string the library owns into Java, optionally freeing it. */
    static String takeString(MemorySegment cString, boolean free) {
        if (cString.address() == 0) {
            return null;
        }
        try {
            return cString.reinterpret(Long.MAX_VALUE).getString(0);
        } finally {
            if (free) {
                free(cString);
            }
        }
    }

    static RuntimeException rethrow(Throwable t) {
        if (t instanceof RuntimeException r) {
            return r;
        }
        if (t instanceof Error e) {
            throw e;
        }
        return new QuartzException("native call failed", t);
    }
}
