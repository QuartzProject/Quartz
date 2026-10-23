package org.quartzlang.jvm;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_BOOLEAN;
import static java.lang.foreign.ValueLayout.JAVA_DOUBLE;
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
    static final int API_LEVEL_NEEDED = 4;
    static final int LOOKUP_FOUND = 0;
    static final int LOOKUP_AMBIGUOUS = 2;
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
    static final StructLayout RULE_LEVEL = MemoryLayout.structLayout(ADDRESS.withName("rule"),
            JAVA_INT.withName("level"), MemoryLayout.paddingLayout(4));
    static final StructLayout ATTRIBUTE_ARG = MemoryLayout.structLayout(ADDRESS.withName("key"),
            JAVA_INT.withName("kind"), MemoryLayout.paddingLayout(4), JAVA_LONG.withName("int_value"),
            JAVA_DOUBLE.withName("float_value"), ADDRESS.withName("text"));
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
    static final MethodHandle SANDBOX_BIND_DIRECT;
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
    static final MethodHandle CALL_STR;
    static final MethodHandle CALL_CHARGE;
    static final MethodHandle CALL_FAIL;
    static final MethodHandle FUNCTION_ARITY;
    static final MethodHandle IMPORT_COUNT;
    static final MethodHandle IMPORT_NAME;
    static final MethodHandle IMPORT_ARITY;
    static final MethodHandle PROGRAM_INTERFACE;
    static final MethodHandle SANDBOX_WAIT_TYPE;
    static final MethodHandle SANDBOX_LIST;
    static final MethodHandle LIST_PUSH;
    static final MethodHandle LIST_LENGTH;
    static final MethodHandle LIST_GET;
    static final MethodHandle SANDBOX_TRAP_CODE;
    static final MethodHandle SANDBOX_RELOAD;
    static final MethodHandle SANDBOX_EXPORT_HANDLE;
    static final MethodHandle SANDBOX_IMPORT_HANDLE;
    static final MethodHandle SANDBOX_DROP_HANDLE;
    static final MethodHandle SANDBOX_LIVE_TASKS;
    static final MethodHandle SANDBOX_FUEL_PER_CALL;
    static final MethodHandle FUNCTION_PARAM_TYPE;
    static final MethodHandle FUNCTION_RESULT_TYPE;
    static final MethodHandle IMPORT_PARAM_TYPE;
    static final MethodHandle IMPORT_RESULT_TYPE;
    static final MethodHandle TYPE_KIND;
    static final MethodHandle TYPE_ELEM;
    static final MethodHandle IMPORT_IS_NATIVE;
    static final MethodHandle SANDBOX_CALL_WIDE;
    static final MethodHandle API_LEVEL;
    static final MethodHandle TYPE_FIELD_COUNT;
    static final MethodHandle TYPE_FIELD;
    static final MethodHandle TYPE_FIELD_NAME;
    static final MethodHandle TYPE_VARIANT_COUNT;
    static final MethodHandle TYPE_VARIANT_NAME;
    static final MethodHandle TYPE_VARIANT_PAYLOAD_COUNT;
    static final MethodHandle TYPE_VARIANT_PAYLOAD;
    static final MethodHandle TYPE_NULLABLE_IS_POINTER;
    static final MethodHandle TYPE_KEY;
    static final MethodHandle SANDBOX_RESUME_WIDE;
    static final MethodHandle SANDBOX_MAP;
    static final MethodHandle MAP_SET;
    static final MethodHandle MAP_LENGTH;
    static final MethodHandle MAP_ENTRY;
    static final MethodHandle FIND_IMPORT;
    static final MethodHandle TYPE_NAME;
    static final MethodHandle SANDBOX_BYTES;
    static final MethodHandle CALL_BYTES;
    static final MethodHandle COMPILE_WITH;
    static final MethodHandle DIAGNOSTICS_FREE;
    static final MethodHandle DIAGNOSTICS_COUNT;
    static final MethodHandle DIAGNOSTICS_TEXT;
    static final MethodHandle DIAGNOSTIC_SEVERITY;
    static final MethodHandle DIAGNOSTIC_RULE;
    static final MethodHandle DIAGNOSTIC_FILE;
    static final MethodHandle DIAGNOSTIC_MESSAGE;
    static final MethodHandle DIAGNOSTIC_RANGE;
    static final MethodHandle WARNING_RULE_COUNT;
    static final MethodHandle WARNING_RULE;
    static final MethodHandle FIND_FUNCTION;
    static final MethodHandle EXPORT_COUNT;
    static final MethodHandle EXPORT;
    static final MethodHandle FUNCTION_NAME;
    static final MethodHandle FUNCTION_PACKAGE;
    static final MethodHandle FUNCTION_PARAM_NAME;
    static final MethodHandle FUNCTION_VISIBILITY;
    static final MethodHandle FUNCTION_IS_SUSPEND;
    static final MethodHandle FUNCTION_IS_EXPORTED;
    static final MethodHandle IMPORT_PACKAGE;
    static final MethodHandle IMPORT_IS_REQUIRED;
    static final MethodHandle TYPE_PACKAGE;
    static final MethodHandle ATTRIBUTE_COUNT;
    static final MethodHandle ATTRIBUTE_NAME;
    static final MethodHandle ATTRIBUTE_FILTER;
    static final MethodHandle ATTRIBUTE_ARG_COUNT;
    static final MethodHandle ATTRIBUTE_ARG_READ;
    static final MethodHandle SANDBOX_TASK_RESULT;

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
        SANDBOX_BIND_DIRECT = fn("qz_sandbox_bind_direct",
                FunctionDescriptor.of(JAVA_BOOLEAN, ADDRESS, ADDRESS, ADDRESS, ADDRESS));
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
        CALL_STR = fn("qz_call_str", FunctionDescriptor.of(JAVA_LONG, JAVA_LONG, ADDRESS, SIZE_T), CRITICAL_HEAP);
        CALL_CHARGE = fn("qz_call_charge", FunctionDescriptor.of(JAVA_BOOLEAN, JAVA_LONG, JAVA_LONG), CRITICAL);
        CALL_FAIL = fn("qz_call_fail", FunctionDescriptor.ofVoid(JAVA_LONG, ADDRESS), CRITICAL);
        FUNCTION_ARITY = fn("qz_program_function_arity", FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT), CRITICAL);
        IMPORT_COUNT = fn("qz_program_import_count", FunctionDescriptor.of(JAVA_INT, ADDRESS), CRITICAL);
        IMPORT_NAME = fn("qz_program_import_name", FunctionDescriptor.of(ADDRESS, ADDRESS, JAVA_INT), CRITICAL);
        IMPORT_ARITY = fn("qz_program_import_arity", FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT), CRITICAL);
        PROGRAM_INTERFACE = fn("qz_program_interface", FunctionDescriptor.of(ADDRESS, ADDRESS, ADDRESS, ADDRESS));
        SANDBOX_WAIT_TYPE = fn("qz_sandbox_wait_type", FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT), CRITICAL);
        SANDBOX_LIST = fn("qz_sandbox_list", FunctionDescriptor.of(JAVA_LONG, ADDRESS, JAVA_INT), CRITICAL);
        LIST_PUSH = fn("qz_list_push", FunctionDescriptor.of(JAVA_BOOLEAN, ADDRESS, JAVA_LONG, ADDRESS, SIZE_T),
                CRITICAL);
        LIST_LENGTH = fn("qz_list_length", FunctionDescriptor.of(SIZE_T, JAVA_LONG), CRITICAL);
        LIST_GET = fn("qz_list_get", FunctionDescriptor.of(JAVA_BOOLEAN, JAVA_LONG, SIZE_T, ADDRESS, SIZE_T),
                CRITICAL);
        SANDBOX_TRAP_CODE = fn("qz_sandbox_trap_code", FunctionDescriptor.of(JAVA_INT, ADDRESS), CRITICAL);
        SANDBOX_RELOAD = fn("qz_sandbox_reload", FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS));
        SANDBOX_EXPORT_HANDLE = fn("qz_sandbox_export_handle", FunctionDescriptor.of(ADDRESS, ADDRESS, JAVA_LONG));
        SANDBOX_IMPORT_HANDLE = fn("qz_sandbox_import_handle",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_LONG, ADDRESS, SIZE_T));
        SANDBOX_DROP_HANDLE = fn("qz_sandbox_drop_handle", FunctionDescriptor.ofVoid(ADDRESS, JAVA_LONG));
        SANDBOX_LIVE_TASKS = fn("qz_sandbox_live_tasks", FunctionDescriptor.of(SIZE_T, ADDRESS), CRITICAL);
        SANDBOX_FUEL_PER_CALL = fn("qz_sandbox_fuel_per_call", FunctionDescriptor.of(JAVA_LONG, ADDRESS), CRITICAL);
        FUNCTION_PARAM_TYPE = fn("qz_program_function_param_type",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT, JAVA_INT), CRITICAL);
        FUNCTION_RESULT_TYPE = fn("qz_program_function_result_type", FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT),
                CRITICAL);
        IMPORT_PARAM_TYPE = fn("qz_program_import_param_type",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT, JAVA_INT), CRITICAL);
        IMPORT_RESULT_TYPE = fn("qz_program_import_result_type", FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT),
                CRITICAL);
        TYPE_KIND = fn("qz_type_kind", FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT, ADDRESS), CRITICAL);
        TYPE_ELEM = fn("qz_type_elem", FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT), CRITICAL);
        IMPORT_IS_NATIVE = fn("qz_program_import_is_native", FunctionDescriptor.of(JAVA_BOOLEAN, ADDRESS, JAVA_INT),
                CRITICAL);
        SANDBOX_CALL_WIDE = fn("qz_sandbox_call_wide",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT, ADDRESS, SIZE_T, ADDRESS, SIZE_T));
        API_LEVEL = fn("qz_api_level", FunctionDescriptor.of(JAVA_INT), CRITICAL);
        TYPE_FIELD_COUNT = fn("qz_type_field_count", FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT), CRITICAL);
        TYPE_FIELD = fn("qz_type_field", FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT, JAVA_INT, ADDRESS),
                CRITICAL);
        TYPE_FIELD_NAME = fn("qz_type_field_name", FunctionDescriptor.of(ADDRESS, ADDRESS, JAVA_INT, JAVA_INT),
                CRITICAL);
        TYPE_VARIANT_COUNT = fn("qz_type_variant_count", FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT), CRITICAL);
        TYPE_VARIANT_NAME = fn("qz_type_variant_name", FunctionDescriptor.of(ADDRESS, ADDRESS, JAVA_INT, JAVA_INT),
                CRITICAL);
        TYPE_VARIANT_PAYLOAD_COUNT = fn("qz_type_variant_payload_count",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT, JAVA_INT), CRITICAL);
        TYPE_VARIANT_PAYLOAD = fn("qz_type_variant_payload",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT, JAVA_INT, JAVA_INT, ADDRESS), CRITICAL);
        TYPE_NULLABLE_IS_POINTER = fn("qz_type_nullable_is_pointer",
                FunctionDescriptor.of(JAVA_BOOLEAN, ADDRESS, JAVA_INT), CRITICAL);
        TYPE_KEY = fn("qz_type_key", FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT), CRITICAL);
        SANDBOX_RESUME_WIDE = fn("qz_sandbox_resume_wide",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT, ADDRESS, SIZE_T, ADDRESS));
        SANDBOX_MAP = fn("qz_sandbox_map", FunctionDescriptor.of(JAVA_LONG, ADDRESS, JAVA_INT), CRITICAL);
        MAP_SET = fn("qz_map_set", FunctionDescriptor.of(JAVA_BOOLEAN, ADDRESS, JAVA_LONG, ADDRESS, ADDRESS), CRITICAL);
        MAP_LENGTH = fn("qz_map_length", FunctionDescriptor.of(SIZE_T, JAVA_LONG), CRITICAL);
        MAP_ENTRY = fn("qz_map_entry",
                FunctionDescriptor.of(JAVA_BOOLEAN, ADDRESS, JAVA_LONG, SIZE_T, ADDRESS, ADDRESS), CRITICAL);
        FIND_IMPORT = fn("qz_program_find_import", FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, ADDRESS),
                CRITICAL);
        TYPE_NAME = fn("qz_type_name", FunctionDescriptor.of(ADDRESS, ADDRESS, JAVA_INT), CRITICAL);
        SANDBOX_BYTES = fn("qz_sandbox_bytes", FunctionDescriptor.of(JAVA_LONG, ADDRESS, ADDRESS, SIZE_T),
                CRITICAL_HEAP);
        CALL_BYTES = fn("qz_call_bytes", FunctionDescriptor.of(JAVA_LONG, JAVA_LONG, ADDRESS, SIZE_T), CRITICAL_HEAP);
        COMPILE_WITH = fn("qz_compile_with",
                FunctionDescriptor.of(ADDRESS, ADDRESS, SIZE_T, ADDRESS, ADDRESS, SIZE_T, ADDRESS));
        DIAGNOSTICS_FREE = fn("qz_diagnostics_free", FunctionDescriptor.ofVoid(ADDRESS));
        DIAGNOSTICS_COUNT = fn("qz_diagnostics_count", FunctionDescriptor.of(SIZE_T, ADDRESS), CRITICAL);
        DIAGNOSTICS_TEXT = fn("qz_diagnostics_text", FunctionDescriptor.of(ADDRESS, ADDRESS), CRITICAL);
        DIAGNOSTIC_SEVERITY = fn("qz_diagnostic_severity", FunctionDescriptor.of(JAVA_INT, ADDRESS, SIZE_T), CRITICAL);
        DIAGNOSTIC_RULE = fn("qz_diagnostic_rule", FunctionDescriptor.of(ADDRESS, ADDRESS, SIZE_T), CRITICAL);
        DIAGNOSTIC_FILE = fn("qz_diagnostic_file", FunctionDescriptor.of(ADDRESS, ADDRESS, SIZE_T), CRITICAL);
        DIAGNOSTIC_MESSAGE = fn("qz_diagnostic_message", FunctionDescriptor.of(ADDRESS, ADDRESS, SIZE_T), CRITICAL);
        DIAGNOSTIC_RANGE = fn("qz_diagnostic_range",
                FunctionDescriptor.ofVoid(ADDRESS, SIZE_T, ADDRESS, ADDRESS, ADDRESS, ADDRESS), CRITICAL);
        WARNING_RULE_COUNT = fn("qz_warning_rule_count", FunctionDescriptor.of(SIZE_T), CRITICAL);
        WARNING_RULE = fn("qz_warning_rule", FunctionDescriptor.of(ADDRESS, SIZE_T), CRITICAL);
        FIND_FUNCTION = fn("qz_program_find_function", FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, ADDRESS),
                CRITICAL);
        EXPORT_COUNT = fn("qz_program_export_count", FunctionDescriptor.of(JAVA_INT, ADDRESS), CRITICAL);
        EXPORT = fn("qz_program_export", FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT), CRITICAL);
        FUNCTION_NAME = fn("qz_program_function_name", FunctionDescriptor.of(ADDRESS, ADDRESS, JAVA_INT), CRITICAL);
        FUNCTION_PACKAGE = fn("qz_program_function_package", FunctionDescriptor.of(ADDRESS, ADDRESS, JAVA_INT),
                CRITICAL);
        FUNCTION_PARAM_NAME = fn("qz_program_function_param_name",
                FunctionDescriptor.of(ADDRESS, ADDRESS, JAVA_INT, JAVA_INT), CRITICAL);
        FUNCTION_VISIBILITY = fn("qz_program_function_visibility", FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT),
                CRITICAL);
        FUNCTION_IS_SUSPEND = fn("qz_program_function_is_suspend",
                FunctionDescriptor.of(JAVA_BOOLEAN, ADDRESS, JAVA_INT), CRITICAL);
        FUNCTION_IS_EXPORTED = fn("qz_program_function_is_exported",
                FunctionDescriptor.of(JAVA_BOOLEAN, ADDRESS, JAVA_INT), CRITICAL);
        IMPORT_PACKAGE = fn("qz_program_import_package", FunctionDescriptor.of(ADDRESS, ADDRESS, JAVA_INT), CRITICAL);
        IMPORT_IS_REQUIRED = fn("qz_program_import_is_required",
                FunctionDescriptor.of(JAVA_BOOLEAN, ADDRESS, JAVA_INT), CRITICAL);
        TYPE_PACKAGE = fn("qz_type_package", FunctionDescriptor.of(ADDRESS, ADDRESS, JAVA_INT), CRITICAL);
        ATTRIBUTE_COUNT = fn("qz_program_function_attribute_count",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT), CRITICAL);
        ATTRIBUTE_NAME = fn("qz_program_function_attribute_name",
                FunctionDescriptor.of(ADDRESS, ADDRESS, JAVA_INT, JAVA_INT), CRITICAL);
        ATTRIBUTE_FILTER = fn("qz_program_function_attribute_filter",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT, JAVA_INT), CRITICAL);
        ATTRIBUTE_ARG_COUNT = fn("qz_program_function_attribute_arg_count",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT, JAVA_INT), CRITICAL);
        ATTRIBUTE_ARG_READ = fn("qz_program_function_attribute_arg",
                FunctionDescriptor.of(JAVA_BOOLEAN, ADDRESS, JAVA_INT, JAVA_INT, JAVA_INT, ADDRESS), CRITICAL);
        SANDBOX_TASK_RESULT = fn("qz_sandbox_task_result",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT, ADDRESS, SIZE_T), CRITICAL);
        int abi = abiVersion();
        if (abi != ABI_VERSION) {
            throw new UnsatisfiedLinkError("libquartz has ABI " + abi + ", quartz-jvm needs " + ABI_VERSION);
        }
        int level = apiLevel();
        if (level < API_LEVEL_NEEDED) {
            throw new UnsatisfiedLinkError("libquartz has API level " + level + ", quartz-jvm needs "
                    + API_LEVEL_NEEDED);
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

    static int apiLevel() {
        try {
            return (int) API_LEVEL.invokeExact();
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

    /** A NUL-terminated string the library keeps alive, or "" for NULL. */
    static String borrowString(MemorySegment cString) {
        return cString.address() == 0 ? "" : cString.reinterpret(Long.MAX_VALUE).getString(0);
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
