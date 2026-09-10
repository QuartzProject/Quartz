@file:OptIn(ExperimentalForeignApi::class)

package org.quartzlang.kmp

import cnames.structs.qz_call
import cnames.structs.qz_program
import cnames.structs.qz_sandbox
import kotlinx.cinterop.ByteVar
import kotlinx.cinterop.COpaquePointer
import kotlinx.cinterop.CPointed
import kotlinx.cinterop.CPointer
import kotlinx.cinterop.CPointerVar
import kotlinx.cinterop.ExperimentalForeignApi
import kotlinx.cinterop.StableRef
import kotlinx.cinterop.UByteVar
import kotlinx.cinterop.UIntVar
import kotlinx.cinterop.ULongVar
import kotlinx.cinterop.addressOf
import kotlinx.cinterop.alloc
import kotlinx.cinterop.allocArray
import kotlinx.cinterop.asStableRef
import kotlinx.cinterop.cstr
import kotlinx.cinterop.get
import kotlinx.cinterop.memScoped
import kotlinx.cinterop.pointed
import kotlinx.cinterop.ptr
import kotlinx.cinterop.readBytes
import kotlinx.cinterop.reinterpret
import kotlinx.cinterop.set
import kotlinx.cinterop.staticCFunction
import kotlinx.cinterop.toCPointer
import kotlinx.cinterop.toCValues
import kotlinx.cinterop.toKString
import kotlinx.cinterop.toLong
import kotlinx.cinterop.usePinned
import kotlinx.cinterop.value
import org.quartzlang.kmp.cinterop.qz_abi_version
import org.quartzlang.kmp.cinterop.qz_call_charge
import org.quartzlang.kmp.cinterop.qz_call_fail
import org.quartzlang.kmp.cinterop.qz_compile
import org.quartzlang.kmp.cinterop.qz_compile_options
import org.quartzlang.kmp.cinterop.qz_free
import org.quartzlang.kmp.cinterop.qz_limits
import org.quartzlang.kmp.cinterop.qz_load
import org.quartzlang.kmp.cinterop.qz_program_binary
import org.quartzlang.kmp.cinterop.qz_program_free
import org.quartzlang.kmp.cinterop.qz_program_function
import org.quartzlang.kmp.cinterop.qz_program_function_arity
import org.quartzlang.kmp.cinterop.qz_program_function_param_type
import org.quartzlang.kmp.cinterop.qz_program_function_result_type
import org.quartzlang.kmp.cinterop.qz_program_import_arity
import org.quartzlang.kmp.cinterop.qz_program_import_count
import org.quartzlang.kmp.cinterop.qz_program_import_is_native
import org.quartzlang.kmp.cinterop.qz_program_import_name
import org.quartzlang.kmp.cinterop.qz_program_import_param_type
import org.quartzlang.kmp.cinterop.qz_program_import_result_type
import org.quartzlang.kmp.cinterop.qz_program_interface
import org.quartzlang.kmp.cinterop.qz_sandbox_bind_direct
import org.quartzlang.kmp.cinterop.qz_sandbox_call
import org.quartzlang.kmp.cinterop.qz_sandbox_call_wide
import org.quartzlang.kmp.cinterop.qz_sandbox_cancel
import org.quartzlang.kmp.cinterop.qz_sandbox_drop_handle
import org.quartzlang.kmp.cinterop.qz_sandbox_error
import org.quartzlang.kmp.cinterop.qz_sandbox_export_handle
import org.quartzlang.kmp.cinterop.qz_sandbox_export_state
import org.quartzlang.kmp.cinterop.qz_sandbox_free
import org.quartzlang.kmp.cinterop.qz_sandbox_fuel_left
import org.quartzlang.kmp.cinterop.qz_sandbox_fuel_per_call
import org.quartzlang.kmp.cinterop.qz_sandbox_import_handle
import org.quartzlang.kmp.cinterop.qz_sandbox_import_state
import org.quartzlang.kmp.cinterop.qz_sandbox_link
import org.quartzlang.kmp.cinterop.qz_sandbox_live_tasks
import org.quartzlang.kmp.cinterop.qz_sandbox_memory_used
import org.quartzlang.kmp.cinterop.qz_sandbox_new
import org.quartzlang.kmp.cinterop.qz_sandbox_reload
import org.quartzlang.kmp.cinterop.qz_sandbox_resume_wide
import org.quartzlang.kmp.cinterop.qz_sandbox_set_fuel
import org.quartzlang.kmp.cinterop.qz_sandbox_spawn
import org.quartzlang.kmp.cinterop.qz_sandbox_start
import org.quartzlang.kmp.cinterop.qz_sandbox_trap_code
import org.quartzlang.kmp.cinterop.qz_sandbox_wait_type
import org.quartzlang.kmp.cinterop.qz_slot
import org.quartzlang.kmp.cinterop.qz_source
import org.quartzlang.kmp.cinterop.qz_task_outcome
import org.quartzlang.kmp.cinterop.qz_type_elem
import org.quartzlang.kmp.cinterop.qz_type_field
import org.quartzlang.kmp.cinterop.qz_type_field_count
import org.quartzlang.kmp.cinterop.qz_type_field_name
import org.quartzlang.kmp.cinterop.qz_type_key
import org.quartzlang.kmp.cinterop.qz_type_kind
import org.quartzlang.kmp.cinterop.qz_type_nullable_is_pointer
import org.quartzlang.kmp.cinterop.qz_type_variant_count
import org.quartzlang.kmp.cinterop.qz_type_variant_name
import org.quartzlang.kmp.cinterop.qz_type_variant_payload
import org.quartzlang.kmp.cinterop.qz_type_variant_payload_count
import org.quartzlang.kmp.cinterop.qzk_call_str
import org.quartzlang.kmp.cinterop.qzk_list_get
import org.quartzlang.kmp.cinterop.qzk_list_length
import org.quartzlang.kmp.cinterop.qzk_list_push
import org.quartzlang.kmp.cinterop.qzk_map_entry
import org.quartzlang.kmp.cinterop.qzk_map_length
import org.quartzlang.kmp.cinterop.qzk_map_set
import org.quartzlang.kmp.cinterop.qzk_resume
import org.quartzlang.kmp.cinterop.qzk_sandbox_list
import org.quartzlang.kmp.cinterop.qzk_sandbox_map
import org.quartzlang.kmp.cinterop.qzk_sandbox_str
import org.quartzlang.kmp.cinterop.qzk_str

private fun program(p: Long): CPointer<qz_program> = p.toCPointer()!!

private fun sandbox(s: Long): CPointer<qz_sandbox> = s.toCPointer()!!

private fun qzCallOf(c: Long): CPointer<qz_call> = c.toCPointer()!!

/** Copies a string the library allocated into Kotlin and frees it. */
private fun take(text: CPointer<ByteVar>?): String? = text?.let {
    try {
        it.toKString()
    } finally {
        qz_free(it)
    }
}

private fun pack(kind: UInt, slots: UInt): Long = (kind.toLong() shl 32) or slots.toLong()

internal actual suspend fun loadRuntime() {}

internal actual fun qzAbiVersion(): Int = qz_abi_version().toInt()

internal actual fun qzCompile(
    names: Array<String>,
    texts: Array<String>,
    emitBinary: Boolean,
    keepLines: Boolean,
    manifest: String?,
    diagnostics: Array<String?>,
): Long = memScoped {
    val sources = allocArray<qz_source>(maxOf(1, names.size))
    for (i in names.indices) {
        val utf8 = texts[i].encodeToByteArray()
        sources[i].name = names[i].cstr.ptr
        val text = allocArray<ByteVar>(maxOf(1, utf8.size))
        utf8.forEachIndexed { k, v -> text[k] = v }
        sources[i].text = text
        sources[i].length = utf8.size.toULong()
    }
    val options = alloc<qz_compile_options>()
    options.emit_binary = emitBinary
    options.keep_lines = keepLines
    options.manifest = manifest?.cstr?.ptr
    val out = alloc<CPointerVar<ByteVar>>()
    val p = qz_compile(sources, names.size.toULong(), options.ptr, out.ptr)
    diagnostics[0] = take(out.value)
    p.toLong()
}

internal actual fun qzLoad(
    bytes: ByteArray,
    keys: Array<ByteArray>,
    requireSignature: Boolean,
    error: Array<String?>,
): Long = memScoped {
    val module = allocArray<UByteVar>(maxOf(1, bytes.size))
    bytes.forEachIndexed { i, b -> module[i] = b.toUByte() }
    val keyBytes = allocArray<UByteVar>(maxOf(1, keys.size * 32))
    keys.forEachIndexed { k, key -> key.forEachIndexed { i, b -> keyBytes[k * 32 + i] = b.toUByte() } }
    val out = alloc<CPointerVar<ByteVar>>()
    val p = qz_load(module, bytes.size.toULong(), if (keys.isEmpty()) null else keyBytes.reinterpret(),
        keys.size.toULong(), requireSignature, out.ptr)
    error[0] = take(out.value)
    p.toLong()
}

internal actual fun qzProgramBinary(program: Long): ByteArray? = memScoped {
    val length = alloc<ULongVar>()
    val bytes = qz_program_binary(program(program), length.ptr) ?: return@memScoped null
    try {
        bytes.readBytes(length.value.toInt())
    } finally {
        qz_free(bytes)
    }
}

internal actual fun qzProgramFree(program: Long) = qz_program_free(program(program))

internal actual fun qzProgramFunction(program: Long, name: String): Int =
    qz_program_function(program(program), name).toInt()

internal actual fun qzFunctionArity(program: Long, function: Int): Int =
    qz_program_function_arity(program(program), function.toUInt()).toInt()

internal actual fun qzFunctionParamType(program: Long, function: Int, index: Int): Int =
    qz_program_function_param_type(program(program), function.toUInt(), index.toUInt()).toInt()

internal actual fun qzFunctionResultType(program: Long, function: Int): Int =
    qz_program_function_result_type(program(program), function.toUInt()).toInt()

internal actual fun qzImportCount(program: Long): Int = qz_program_import_count(program(program)).toInt()

internal actual fun qzImportName(program: Long, import: Int): String =
    qz_program_import_name(program(program), import.toUInt())?.toKString() ?: ""

internal actual fun qzImportArity(program: Long, import: Int): Int =
    qz_program_import_arity(program(program), import.toUInt()).toInt()

internal actual fun qzImportParamType(program: Long, import: Int, index: Int): Int =
    qz_program_import_param_type(program(program), import.toUInt(), index.toUInt()).toInt()

internal actual fun qzImportResultType(program: Long, import: Int): Int =
    qz_program_import_result_type(program(program), import.toUInt()).toInt()

internal actual fun qzImportIsNative(program: Long, import: Int): Boolean =
    qz_program_import_is_native(program(program), import.toUInt())

internal actual fun qzTypeKind(program: Long, type: Int): Long = memScoped {
    val slots = alloc<UIntVar>()
    pack(qz_type_kind(program(program), type.toUInt(), slots.ptr), slots.value)
}

internal actual fun qzTypeElem(program: Long, type: Int): Int = qz_type_elem(program(program), type.toUInt()).toInt()

internal actual fun qzProgramInterface(program: Long, manifest: String?, error: Array<String?>): String? = memScoped {
    val out = alloc<CPointerVar<ByteVar>>()
    val text = qz_program_interface(program(program), manifest, out.ptr)
    error[0] = take(out.value)
    take(text)
}

internal actual fun qzSandboxNew(program: Long, memory: Long, fuel: Long, depth: Int, tier: Int): Long = memScoped {
    val limits = alloc<qz_limits>()
    limits.memory_bytes = memory.toULong()
    limits.fuel_per_call = fuel
    limits.max_depth = depth.toUInt()
    limits.tier = tier
    qz_sandbox_new(program(program), limits.ptr).toLong()
}

internal actual fun qzSandboxFree(sandbox: Long) = qz_sandbox_free(sandbox(sandbox))

private val trampoline = staticCFunction {
    call: CPointer<qz_call>?, args: CPointer<qz_slot>?, result: CPointer<qz_slot>?, user: COpaquePointer? ->
    user!!.asStableRef<CBinding>().get().dispatch(call.toLong(), args.toLong(), result.toLong())
}

internal actual fun qzBind(sandbox: Long, name: String, binding: CBinding): Long {
    val ref = StableRef.create(binding)
    if (!qz_sandbox_bind_direct(sandbox(sandbox), name, trampoline, ref.asCPointer())) {
        ref.dispose()
        return 0
    }
    return ref.asCPointer().toLong()
}

internal actual fun qzUnbind(token: Long) = token.toCPointer<CPointed>()!!.asStableRef<CBinding>().dispose()

internal actual fun qzStart(sandbox: Long): Int = qz_sandbox_start(sandbox(sandbox)).toInt()

internal actual fun qzCall(sandbox: Long, function: Int, args: LongArray, count: Int, result: LongArray): Int =
    memScoped {
        val out = alloc<ULongVar>()
        val status = args.usePinned { pinned ->
            qz_sandbox_call(sandbox(sandbox), function.toUInt(), pinned.addressOf(0).reinterpret(), count.toULong(),
                out.ptr.reinterpret())
        }
        result[0] = out.value.toLong()
        status.toInt()
    }

internal actual fun qzCallWide(
    sandbox: Long,
    function: Int,
    args: LongArray,
    count: Int,
    result: LongArray,
    slots: Int,
): Int = args.usePinned { pinned ->
    result.usePinned { out ->
        qz_sandbox_call_wide(sandbox(sandbox), function.toUInt(), pinned.addressOf(0).reinterpret(), count.toULong(),
            out.addressOf(0).reinterpret(), slots.toULong()).toInt()
    }
}

private fun store(o: qz_task_outcome, outcome: LongArray) {
    outcome[0] = o.kind.toLong()
    outcome[1] = o.task.toLong()
    outcome[2] = o.value.u.toLong()
}

internal actual fun qzSpawn(sandbox: Long, function: Int, args: LongArray, count: Int, outcome: LongArray): Int =
    memScoped {
        val o = alloc<qz_task_outcome>()
        val status = args.usePinned { pinned ->
            qz_sandbox_spawn(sandbox(sandbox), function.toUInt(), pinned.addressOf(0).reinterpret(), count.toULong(),
                o.ptr)
        }
        store(o, outcome)
        status.toInt()
    }

internal actual fun qzResume(sandbox: Long, task: Int, value: Long, outcome: LongArray): Int = memScoped {
    val o = alloc<qz_task_outcome>()
    val status = qzk_resume(sandbox(sandbox), task.toUInt(), value.toULong(), o.ptr)
    store(o, outcome)
    status.toInt()
}

internal actual fun qzCancel(sandbox: Long, task: Int) = qz_sandbox_cancel(sandbox(sandbox), task.toUInt())

internal actual fun qzError(sandbox: Long): String = qz_sandbox_error(sandbox(sandbox))?.toKString() ?: ""

internal actual fun qzTrapCode(sandbox: Long): Int = qz_sandbox_trap_code(sandbox(sandbox)).toInt()

internal actual fun qzSandboxStr(sandbox: Long, utf8: ByteArray): Long = if (utf8.isEmpty()) {
    qzk_sandbox_str(sandbox(sandbox), "", 0u).toLong()
} else {
    utf8.usePinned { qzk_sandbox_str(sandbox(sandbox), it.addressOf(0), utf8.size.toULong()).toLong() }
}

internal actual fun qzSetFuel(sandbox: Long, fuel: Long) = qz_sandbox_set_fuel(sandbox(sandbox), fuel)

internal actual fun qzFuelLeft(sandbox: Long): Long = qz_sandbox_fuel_left(sandbox(sandbox))

internal actual fun qzFuelPerCall(sandbox: Long): Long = qz_sandbox_fuel_per_call(sandbox(sandbox))

internal actual fun qzMemoryUsed(sandbox: Long): Long = qz_sandbox_memory_used(sandbox(sandbox)).toLong()

internal actual fun qzLiveTasks(sandbox: Long): Long = qz_sandbox_live_tasks(sandbox(sandbox)).toLong()

internal actual fun qzExportState(sandbox: Long): String? = take(qz_sandbox_export_state(sandbox(sandbox)))

internal actual fun qzImportState(sandbox: Long, json: String): Int {
    val utf8 = json.encodeToByteArray()
    return memScoped {
        qz_sandbox_import_state(sandbox(sandbox), utf8.toCValues().ptr, utf8.size.toULong()).toInt()
    }
}

internal actual fun qzLink(client: Long, pkg: String, provider: Long): Int =
    qz_sandbox_link(sandbox(client), pkg, sandbox(provider)).toInt()

internal actual fun qzReload(sandbox: Long, program: Long): Int =
    qz_sandbox_reload(sandbox(sandbox), program(program)).toInt()

internal actual fun qzExportHandle(sandbox: Long, handle: Long): String? =
    take(qz_sandbox_export_handle(sandbox(sandbox), handle.toULong()))

internal actual fun qzImportHandle(sandbox: Long, handle: Long, json: String): Int {
    val utf8 = json.encodeToByteArray()
    return memScoped {
        qz_sandbox_import_handle(sandbox(sandbox), handle.toULong(), utf8.toCValues().ptr, utf8.size.toULong()).toInt()
    }
}

internal actual fun qzDropHandle(sandbox: Long, handle: Long) =
    qz_sandbox_drop_handle(sandbox(sandbox), handle.toULong())

internal actual fun qzStr(slot: Long): ByteArray = memScoped {
    val length = alloc<ULongVar>()
    val text = qzk_str(slot.toULong(), length.ptr) ?: return@memScoped ByteArray(0)
    if (length.value == 0uL) ByteArray(0) else text.readBytes(length.value.toInt())
}

internal actual fun qzSlot(args: Long, index: Int): Long = args.toCPointer<ULongVar>()!![index].toLong()

internal actual fun qzCallStr(call: Long, utf8: ByteArray): Long = if (utf8.isEmpty()) {
    qzk_call_str(qzCallOf(call), "", 0u).toLong()
} else {
    utf8.usePinned { qzk_call_str(qzCallOf(call), it.addressOf(0), utf8.size.toULong()).toLong() }
}

internal actual fun qzSetSlot(address: Long, index: Int, bits: Long) {
    address.toCPointer<ULongVar>()!![index] = bits.toULong()
}

internal actual fun qzCallCharge(call: Long, units: Long): Boolean = qz_call_charge(qzCallOf(call), units)

internal actual fun qzCallFail(call: Long, message: String) = qz_call_fail(qzCallOf(call), message)

internal actual fun qzWaitType(sandbox: Long, task: Int): Int =
    qz_sandbox_wait_type(sandbox(sandbox), task.toUInt()).toInt()

internal actual fun qzSandboxList(sandbox: Long, type: Int): Long =
    qzk_sandbox_list(sandbox(sandbox), type.toUInt()).toLong()

internal actual fun qzListPush(sandbox: Long, list: Long, element: LongArray, slots: Int): Boolean =
    element.usePinned {
        qzk_list_push(sandbox(sandbox), list.toULong(), it.addressOf(0).reinterpret(), slots.toULong())
    }

internal actual fun qzListLength(list: Long): Long = qzk_list_length(list.toULong()).toLong()

internal actual fun qzListGet(list: Long, index: Long, out: LongArray, slots: Int): Boolean = out.usePinned {
    qzk_list_get(list.toULong(), index.toULong(), it.addressOf(0).reinterpret(), slots.toULong())
}

internal actual fun qzTypeFieldCount(program: Long, type: Int): Int =
    qz_type_field_count(program(program), type.toUInt()).toInt()

internal actual fun qzTypeField(program: Long, type: Int, index: Int): Long = memScoped {
    val offset = alloc<UIntVar>()
    pack(qz_type_field(program(program), type.toUInt(), index.toUInt(), offset.ptr), offset.value)
}

internal actual fun qzTypeFieldName(program: Long, type: Int, index: Int): String =
    qz_type_field_name(program(program), type.toUInt(), index.toUInt())?.toKString() ?: ""

internal actual fun qzTypeVariantCount(program: Long, type: Int): Int =
    qz_type_variant_count(program(program), type.toUInt()).toInt()

internal actual fun qzTypeVariantName(program: Long, type: Int, variant: Int): String =
    qz_type_variant_name(program(program), type.toUInt(), variant.toUInt())?.toKString() ?: ""

internal actual fun qzTypeVariantPayloadCount(program: Long, type: Int, variant: Int): Int =
    qz_type_variant_payload_count(program(program), type.toUInt(), variant.toUInt()).toInt()

internal actual fun qzTypeVariantPayload(program: Long, type: Int, variant: Int, index: Int): Long = memScoped {
    val offset = alloc<UIntVar>()
    pack(qz_type_variant_payload(program(program), type.toUInt(), variant.toUInt(), index.toUInt(), offset.ptr),
        offset.value)
}

internal actual fun qzTypeNullableIsPointer(program: Long, type: Int): Boolean =
    qz_type_nullable_is_pointer(program(program), type.toUInt())

internal actual fun qzTypeKey(program: Long, type: Int): Int = qz_type_key(program(program), type.toUInt()).toInt()

internal actual fun qzResumeWide(sandbox: Long, task: Int, value: LongArray, slots: Int, outcome: LongArray): Int =
    memScoped {
        val o = alloc<qz_task_outcome>()
        val status = value.usePinned { pinned ->
            qz_sandbox_resume_wide(sandbox(sandbox), task.toUInt(), pinned.addressOf(0).reinterpret(),
                slots.toULong(), o.ptr)
        }
        store(o, outcome)
        status.toInt()
    }

internal actual fun qzSandboxMap(sandbox: Long, type: Int): Long =
    qzk_sandbox_map(sandbox(sandbox), type.toUInt()).toLong()

internal actual fun qzMapSet(sandbox: Long, map: Long, key: LongArray, value: LongArray): Boolean =
    key.usePinned { k ->
        value.usePinned { v ->
            qzk_map_set(sandbox(sandbox), map.toULong(), k.addressOf(0).reinterpret(), v.addressOf(0).reinterpret())
        }
    }

internal actual fun qzMapLength(map: Long): Long = qzk_map_length(map.toULong()).toLong()

internal actual fun qzMapEntry(sandbox: Long, map: Long, index: Long, key: LongArray, value: LongArray): Boolean =
    key.usePinned { k ->
        value.usePinned { v ->
            qzk_map_entry(sandbox(sandbox), map.toULong(), index.toULong(), k.addressOf(0).reinterpret(),
                v.addressOf(0).reinterpret())
        }
    }
