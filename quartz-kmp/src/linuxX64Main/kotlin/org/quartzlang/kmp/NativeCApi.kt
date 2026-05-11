@file:OptIn(ExperimentalForeignApi::class)

package org.quartzlang.kmp

import kotlinx.cinterop.ByteVar
import kotlinx.cinterop.COpaquePointer
import kotlinx.cinterop.CPointer
import kotlinx.cinterop.CPointerVar
import kotlinx.cinterop.ExperimentalForeignApi
import kotlinx.cinterop.StableRef
import kotlinx.cinterop.UByteVar
import kotlinx.cinterop.ULongVar
import kotlinx.cinterop.UIntVar
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
import cnames.structs.qz_call
import org.quartzlang.kmp.cinterop.qz_call_charge
import org.quartzlang.kmp.cinterop.qz_call_fail
import org.quartzlang.kmp.cinterop.qz_compile
import org.quartzlang.kmp.cinterop.qz_compile_options
import org.quartzlang.kmp.cinterop.qz_free
import org.quartzlang.kmp.cinterop.qz_limits
import org.quartzlang.kmp.cinterop.qz_load
import cnames.structs.qz_program
import org.quartzlang.kmp.cinterop.qz_program_binary
import org.quartzlang.kmp.cinterop.qz_program_free
import org.quartzlang.kmp.cinterop.qz_program_function
import org.quartzlang.kmp.cinterop.qz_program_function_arity
import org.quartzlang.kmp.cinterop.qz_program_function_param
import org.quartzlang.kmp.cinterop.qz_program_function_result
import org.quartzlang.kmp.cinterop.qz_program_import_arity
import org.quartzlang.kmp.cinterop.qz_program_import_count
import org.quartzlang.kmp.cinterop.qz_program_import_name
import org.quartzlang.kmp.cinterop.qz_program_import_param
import org.quartzlang.kmp.cinterop.qz_program_import_result
import org.quartzlang.kmp.cinterop.qz_program_interface
import cnames.structs.qz_sandbox
import org.quartzlang.kmp.cinterop.qz_sandbox_bind_direct
import org.quartzlang.kmp.cinterop.qz_sandbox_call
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
import org.quartzlang.kmp.cinterop.qz_sandbox_set_fuel
import org.quartzlang.kmp.cinterop.qz_sandbox_spawn
import org.quartzlang.kmp.cinterop.qz_sandbox_start
import org.quartzlang.kmp.cinterop.qz_sandbox_trap_code
import org.quartzlang.kmp.cinterop.qz_sandbox_wait_type
import org.quartzlang.kmp.cinterop.qz_slot
import org.quartzlang.kmp.cinterop.qz_source
import org.quartzlang.kmp.cinterop.qz_task_outcome
import org.quartzlang.kmp.cinterop.qzk_call_set_result
import org.quartzlang.kmp.cinterop.qzk_call_str
import org.quartzlang.kmp.cinterop.qzk_list_get
import org.quartzlang.kmp.cinterop.qzk_list_length
import org.quartzlang.kmp.cinterop.qzk_list_push
import org.quartzlang.kmp.cinterop.qzk_resume
import org.quartzlang.kmp.cinterop.qzk_sandbox_list
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
        sources[i].text = allocArray<ByteVar>(maxOf(1, utf8.size)).also { b -> utf8.forEachIndexed { k, v -> b[k] = v } }
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

internal actual fun qzLoad(bytes: ByteArray, keys: Array<ByteArray>, requireSignature: Boolean, error: Array<String?>): Long =
    memScoped {
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

internal actual fun qzFunctionParam(program: Long, function: Int, index: Int): Long = memScoped {
    val slots = alloc<UIntVar>()
    pack(qz_program_function_param(program(program), function.toUInt(), index.toUInt(), slots.ptr), slots.value)
}

internal actual fun qzFunctionResult(program: Long, function: Int): Long = memScoped {
    val slots = alloc<UIntVar>()
    pack(qz_program_function_result(program(program), function.toUInt(), slots.ptr), slots.value)
}

internal actual fun qzImportCount(program: Long): Int = qz_program_import_count(program(program)).toInt()

internal actual fun qzImportName(program: Long, import: Int): String =
    qz_program_import_name(program(program), import.toUInt())?.toKString() ?: ""

internal actual fun qzImportArity(program: Long, import: Int): Int =
    qz_program_import_arity(program(program), import.toUInt()).toInt()

internal actual fun qzImportParam(program: Long, import: Int, index: Int): Long = memScoped {
    val slots = alloc<UIntVar>()
    pack(qz_program_import_param(program(program), import.toUInt(), index.toUInt(), slots.ptr), slots.value)
}

internal actual fun qzImportResult(program: Long, import: Int): Long = memScoped {
    val slots = alloc<UIntVar>()
    pack(qz_program_import_result(program(program), import.toUInt(), slots.ptr), slots.value)
}

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
    call: CPointer<qz_call>?, args: CPointer<qz_slot>?, _: CPointer<qz_slot>?, user: COpaquePointer? ->
    user!!.asStableRef<CBinding>().get().dispatch(call.toLong(), args.toLong())
}

internal actual fun qzBind(sandbox: Long, name: String, binding: CBinding): Long {
    val ref = StableRef.create(binding)
    if (!qz_sandbox_bind_direct(sandbox(sandbox), name, trampoline, ref.asCPointer())) {
        ref.dispose()
        return 0
    }
    return ref.asCPointer().toLong()
}

internal actual fun qzUnbind(token: Long) = token.toCPointer<kotlinx.cinterop.CPointed>()!!.asStableRef<CBinding>().dispose()

internal actual fun qzStart(sandbox: Long): Int = qz_sandbox_start(sandbox(sandbox)).toInt()

internal actual fun qzCall(sandbox: Long, function: Int, args: LongArray, count: Int, result: LongArray): Int = memScoped {
    val out = alloc<ULongVar>()
    val status = args.usePinned { pinned ->
        qz_sandbox_call(sandbox(sandbox), function.toUInt(), pinned.addressOf(0).reinterpret(), count.toULong(),
            out.ptr.reinterpret())
    }
    result[0] = out.value.toLong()
    status.toInt()
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

internal actual fun qzDropHandle(sandbox: Long, handle: Long) = qz_sandbox_drop_handle(sandbox(sandbox), handle.toULong())

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

internal actual fun qzCallSetResult(call: Long, bits: Long) = qzk_call_set_result(qzCallOf(call), bits.toULong())

internal actual fun qzCallCharge(call: Long, units: Long): Boolean = qz_call_charge(qzCallOf(call), units)

internal actual fun qzCallFail(call: Long, message: String) = qz_call_fail(qzCallOf(call), message)

internal actual fun qzWaitType(sandbox: Long, task: Int): Int =
    qz_sandbox_wait_type(sandbox(sandbox), task.toUInt()).toInt()

internal actual fun qzSandboxList(sandbox: Long, type: Int): Long =
    qzk_sandbox_list(sandbox(sandbox), type.toUInt()).toLong()

internal actual fun qzListPush(sandbox: Long, list: Long, element: Long): Boolean =
    qzk_list_push(sandbox(sandbox), list.toULong(), element.toULong())

internal actual fun qzListLength(list: Long): Long = qzk_list_length(list.toULong()).toLong()

internal actual fun qzListGet(list: Long, index: Long, out: LongArray): Boolean = memScoped {
    val e = alloc<ULongVar>()
    val ok = qzk_list_get(list.toULong(), index.toULong(), e.ptr)
    out[0] = e.value.toLong()
    ok
}
