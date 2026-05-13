package org.quartzlang.kmp

// quartz.h over quartz-capi.wasm (Quartz's wasm/ build): pointers are 32-bit, 64-bit values travel through memory.

/** Instantiates quartz-capi.wasm once. */
internal expect suspend fun loadModule()

/** Calls the export `_name` of the module with 32-bit arguments (extra ones are ignored). */
internal expect fun wasm(
    name: String,
    a: Int = 0,
    b: Int = 0,
    c: Int = 0,
    d: Int = 0,
    e: Int = 0,
    f: Int = 0,
    g: Int = 0,
): Int

internal expect fun peekU8(address: Int): Int

internal expect fun pokeU8(address: Int, value: Int)

internal expect fun peekI32(address: Int): Int

internal expect fun pokeI32(address: Int, value: Int)

/** Adds the host function trampoline, which calls [webDispatch], to the module's table and returns its index. */
internal expect fun addDispatch(): Int

private const val SCRATCH = 64
private var scratch = 0
private var dispatcher = 0
private val bindings = mutableMapOf<Int, CBinding>()
private var nextBinding = 1

internal actual suspend fun loadRuntime() {
    loadModule()
    if (scratch == 0) {
        scratch = wasm("malloc", SCRATCH)
        dispatcher = addDispatch()
    }
}

/** What the module's table entry for host functions calls; never throws. */
internal fun webDispatch(call: Int, args: Int, result: Int, user: Int): Int {
    val binding = bindings[user] ?: return 0
    return if (binding.dispatch(addr(call), addr(args), addr(result))) 1 else 0
}

private fun addr(p: Int): Long = p.toLong() and 0xFFFF_FFFFL

private fun p(address: Long): Int = address.toInt()

private fun malloc(size: Int): Int = wasm("malloc", maxOf(size, 1))

private fun free(address: Int) {
    if (address != 0) {
        wasm("free", address)
    }
}

private fun peek64(address: Int): Long =
    (peekI32(address).toLong() and 0xFFFF_FFFFL) or (peekI32(address + 4).toLong() shl 32)

private fun poke64(address: Int, value: Long) {
    pokeI32(address, value.toInt())
    pokeI32(address + 4, (value ushr 32).toInt())
}

private fun bytesIn(bytes: ByteArray, nul: Boolean = false): Int {
    val address = malloc(bytes.size + 1)
    bytes.forEachIndexed { i, b -> pokeU8(address + i, b.toInt()) }
    if (nul) {
        pokeU8(address + bytes.size, 0)
    }
    return address
}

private fun cString(text: String): Int = bytesIn(text.encodeToByteArray(), nul = true)

private fun bytesOut(address: Int, length: Int): ByteArray = ByteArray(length) { peekU8(address + it).toByte() }

private fun stringAt(address: Int): String {
    var n = 0
    while (peekU8(address + n) != 0) {
        n++
    }
    return bytesOut(address, n).decodeToString()
}

/** Copies a string the library allocated and frees it. */
private fun take(address: Int): String? =
    if (address == 0) null else stringAt(address).also { wasm("qz_free", address) }

private inline fun <R> withBuffer(size: Int, use: (Int) -> R): R {
    val buffer = malloc(size)
    try {
        return use(buffer)
    } finally {
        free(buffer)
    }
}

private inline fun <R> withText(text: String?, use: (Int, Int) -> R): R {
    if (text == null) {
        return use(0, 0)
    }
    val bytes = text.encodeToByteArray()
    val address = bytesIn(bytes, nul = true)
    try {
        return use(address, bytes.size)
    } finally {
        free(address)
    }
}

private fun slotsIn(values: LongArray, count: Int): Int {
    val address = malloc(8 * count)
    for (i in 0 until count) {
        poke64(address + 8 * i, values[i])
    }
    return address
}

internal actual fun qzAbiVersion(): Int = wasm("qz_abi_version")

internal actual fun qzCompile(
    names: Array<String>,
    texts: Array<String>,
    emitBinary: Boolean,
    keepLines: Boolean,
    manifest: String?,
    diagnostics: Array<String?>,
): Long {
    val n = names.size
    val owned = mutableListOf<Int>()
    val namePtrs = malloc(4 * n).also { owned += it }
    val textPtrs = malloc(4 * n).also { owned += it }
    val lengths = malloc(4 * n).also { owned += it }
    for (i in 0 until n) {
        val text = texts[i].encodeToByteArray()
        pokeI32(namePtrs + 4 * i, cString(names[i]).also { owned += it })
        pokeI32(textPtrs + 4 * i, bytesIn(text).also { owned += it })
        pokeI32(lengths + 4 * i, text.size)
    }
    val m = if (manifest == null) 0 else cString(manifest).also { owned += it }
    val flags = (if (emitBinary) 1 else 0) or (if (keepLines) 2 else 0)
    pokeI32(scratch, 0)
    val program = wasm("qzw_compile", namePtrs, textPtrs, lengths, n, flags, m, scratch)
    owned.forEach(::free)
    diagnostics[0] = take(peekI32(scratch))
    return addr(program)
}

internal actual fun qzLoad(bytes: ByteArray, keys: Array<ByteArray>, requireSignature: Boolean, error: Array<String?>):
    Long {
    val module = bytesIn(bytes)
    val keyBytes = bytesIn(keys.fold(ByteArray(0)) { all, k -> all + k })
    pokeI32(scratch, 0)
    val program = wasm("qz_load", module, bytes.size, if (keys.isEmpty()) 0 else keyBytes, keys.size,
        if (requireSignature) 1 else 0, scratch)
    free(module)
    free(keyBytes)
    error[0] = take(peekI32(scratch))
    return addr(program)
}

internal actual fun qzProgramBinary(program: Long): ByteArray? {
    val data = wasm("qz_program_binary", p(program), scratch)
    if (data == 0) {
        return null
    }
    return bytesOut(data, peekI32(scratch)).also { wasm("qz_free", data) }
}

internal actual fun qzProgramFree(program: Long) {
    wasm("qz_program_free", p(program))
}

internal actual fun qzProgramFunction(program: Long, name: String): Int =
    withText(name) { n, _ -> wasm("qz_program_function", p(program), n) }

internal actual fun qzFunctionArity(program: Long, function: Int): Int =
    wasm("qz_program_function_arity", p(program), function)

internal actual fun qzFunctionParamType(program: Long, function: Int, index: Int): Int =
    wasm("qz_program_function_param_type", p(program), function, index)

internal actual fun qzFunctionResultType(program: Long, function: Int): Int =
    wasm("qz_program_function_result_type", p(program), function)

internal actual fun qzImportCount(program: Long): Int = wasm("qz_program_import_count", p(program))

internal actual fun qzImportName(program: Long, import: Int): String =
    stringAt(wasm("qz_program_import_name", p(program), import))

internal actual fun qzImportArity(program: Long, import: Int): Int = wasm("qz_program_import_arity", p(program), import)

internal actual fun qzImportParamType(program: Long, import: Int, index: Int): Int =
    wasm("qz_program_import_param_type", p(program), import, index)

internal actual fun qzImportResultType(program: Long, import: Int): Int =
    wasm("qz_program_import_result_type", p(program), import)

internal actual fun qzImportIsNative(program: Long, import: Int): Boolean =
    wasm("qz_program_import_is_native", p(program), import) != 0

internal actual fun qzTypeKind(program: Long, type: Int): Long {
    val kind = wasm("qz_type_kind", p(program), type, scratch)
    return (kind.toLong() shl 32) or (peekI32(scratch).toLong() and 0xFFFF_FFFFL)
}

internal actual fun qzTypeElem(program: Long, type: Int): Int = wasm("qz_type_elem", p(program), type)

internal actual fun qzProgramInterface(program: Long, manifest: String?, error: Array<String?>): String? =
    withText(manifest) { m, _ ->
        pokeI32(scratch, 0)
        val text = wasm("qz_program_interface", p(program), m, scratch)
        error[0] = take(peekI32(scratch))
        take(text)
    }

internal actual fun qzSandboxNew(program: Long, memory: Long, fuel: Long, depth: Int, tier: Int): Long {
    poke64(scratch, fuel)
    return addr(wasm("qzw_sandbox_new", p(program), memory.toInt(), scratch, depth, tier))
}

internal actual fun qzSandboxFree(sandbox: Long) {
    wasm("qz_sandbox_free", p(sandbox))
}

internal actual fun qzBind(sandbox: Long, name: String, binding: CBinding): Long {
    val id = nextBinding++
    bindings[id] = binding
    val bound = withText(name) { n, _ -> wasm("qz_sandbox_bind_direct", p(sandbox), n, dispatcher, id) != 0 }
    if (!bound) {
        bindings.remove(id)
        return 0
    }
    return id.toLong()
}

internal actual fun qzUnbind(token: Long) {
    bindings.remove(token.toInt())
}

internal actual fun qzStart(sandbox: Long): Int = wasm("qz_sandbox_start", p(sandbox))

internal actual fun qzCall(sandbox: Long, function: Int, args: LongArray, count: Int, result: LongArray): Int =
    qzCallWide(sandbox, function, args, count, result, 0)

internal actual fun qzCallWide(
    sandbox: Long,
    function: Int,
    args: LongArray,
    count: Int,
    result: LongArray,
    slots: Int,
): Int {
    val a = slotsIn(args, count)
    val width = maxOf(1, slots)
    val out = malloc(8 * width)
    try {
        val status = if (slots == 0) {
            wasm("qz_sandbox_call", p(sandbox), function, a, count, out)
        } else {
            wasm("qz_sandbox_call_wide", p(sandbox), function, a, count, out, slots)
        }
        for (i in 0 until width) {
            result[i] = peek64(out + 8 * i)
        }
        return status
    } finally {
        free(a)
        free(out)
    }
}

private fun storeOutcome(address: Int, outcome: LongArray) {
    outcome[0] = peekI32(address).toLong()
    outcome[1] = peekI32(address + 4).toLong() and 0xFFFF_FFFFL
    outcome[2] = peek64(address + 8)
}

internal actual fun qzSpawn(sandbox: Long, function: Int, args: LongArray, count: Int, outcome: LongArray): Int {
    val a = slotsIn(args, count)
    try {
        return withBuffer(16) { out ->
            wasm("qz_sandbox_spawn", p(sandbox), function, a, count, out).also { storeOutcome(out, outcome) }
        }
    } finally {
        free(a)
    }
}

internal actual fun qzResume(sandbox: Long, task: Int, value: Long, outcome: LongArray): Int = withBuffer(24) { out ->
    poke64(out + 16, value)
    wasm("qzw_resume", p(sandbox), task, out + 16, out).also { storeOutcome(out, outcome) }
}

internal actual fun qzCancel(sandbox: Long, task: Int) {
    wasm("qz_sandbox_cancel", p(sandbox), task)
}

internal actual fun qzError(sandbox: Long): String = stringAt(wasm("qz_sandbox_error", p(sandbox)))

internal actual fun qzTrapCode(sandbox: Long): Int = wasm("qz_sandbox_trap_code", p(sandbox))

internal actual fun qzSandboxStr(sandbox: Long, utf8: ByteArray): Long {
    val data = bytesIn(utf8)
    wasm("qzw_sandbox_str", p(sandbox), data, utf8.size, scratch)
    free(data)
    return peek64(scratch)
}

internal actual fun qzSetFuel(sandbox: Long, fuel: Long) {
    poke64(scratch, fuel)
    wasm("qzw_set_fuel", p(sandbox), scratch)
}

internal actual fun qzFuelLeft(sandbox: Long): Long {
    wasm("qzw_fuel", p(sandbox), scratch)
    return peek64(scratch)
}

internal actual fun qzFuelPerCall(sandbox: Long): Long {
    wasm("qzw_fuel", p(sandbox), scratch)
    return peek64(scratch + 8)
}

internal actual fun qzMemoryUsed(sandbox: Long): Long = addr(wasm("qz_sandbox_memory_used", p(sandbox)))

internal actual fun qzLiveTasks(sandbox: Long): Long = addr(wasm("qz_sandbox_live_tasks", p(sandbox)))

internal actual fun qzExportState(sandbox: Long): String? = take(wasm("qz_sandbox_export_state", p(sandbox)))

internal actual fun qzImportState(sandbox: Long, json: String): Int =
    withText(json) { t, n -> wasm("qz_sandbox_import_state", p(sandbox), t, n) }

internal actual fun qzLink(client: Long, pkg: String, provider: Long): Int =
    withText(pkg) { n, _ -> wasm("qz_sandbox_link", p(client), n, p(provider)) }

internal actual fun qzReload(sandbox: Long, program: Long): Int = wasm("qz_sandbox_reload", p(sandbox), p(program))

internal actual fun qzExportHandle(sandbox: Long, handle: Long): String? {
    poke64(scratch, handle)
    return take(wasm("qzw_export_handle", p(sandbox), scratch))
}

internal actual fun qzImportHandle(sandbox: Long, handle: Long, json: String): Int = withText(json) { t, n ->
    poke64(scratch, handle)
    wasm("qzw_import_handle", p(sandbox), scratch, t, n)
}

internal actual fun qzDropHandle(sandbox: Long, handle: Long) {
    poke64(scratch, handle)
    wasm("qzw_drop_handle", p(sandbox), scratch)
}

internal actual fun qzStr(slot: Long): ByteArray {
    poke64(scratch, slot)
    val data = wasm("qzw_str", scratch, scratch + 8)
    return bytesOut(data, peekI32(scratch + 8))
}

internal actual fun qzSlot(args: Long, index: Int): Long = peek64(p(args) + 8 * index)

internal actual fun qzCallStr(call: Long, utf8: ByteArray): Long {
    val data = bytesIn(utf8)
    wasm("qzw_call_str", p(call), data, utf8.size, scratch + 16)
    free(data)
    return peek64(scratch + 16)
}

internal actual fun qzSetSlot(address: Long, index: Int, bits: Long) = poke64(p(address) + 8 * index, bits)

internal actual fun qzCallCharge(call: Long, units: Long): Boolean {
    poke64(scratch + 24, units)
    return wasm("qzw_call_charge", p(call), scratch + 24) != 0
}

internal actual fun qzCallFail(call: Long, message: String) {
    withText(message) { m, _ -> wasm("qz_call_fail", p(call), m) }
}

internal actual fun qzWaitType(sandbox: Long, task: Int): Int = wasm("qz_sandbox_wait_type", p(sandbox), task)

internal actual fun qzSandboxList(sandbox: Long, type: Int): Long {
    wasm("qzw_sandbox_list", p(sandbox), type, scratch + 32)
    return peek64(scratch + 32)
}

internal actual fun qzListPush(sandbox: Long, list: Long, element: Long): Boolean = withBuffer(16) { cell ->
    poke64(cell, list)
    poke64(cell + 8, element)
    wasm("qzw_list_push", p(sandbox), cell, cell + 8, 1) != 0
}

internal actual fun qzListLength(list: Long): Long {
    poke64(scratch + 40, list)
    return addr(wasm("qzw_list_length", scratch + 40))
}

internal actual fun qzListGet(list: Long, index: Long, out: LongArray, slots: Int): Boolean =
    withBuffer(8 + 8 * slots) { cell ->
        poke64(cell, list)
        val ok = wasm("qzw_list_get", cell, index.toInt(), cell + 8, slots) != 0
        for (i in 0 until slots) {
            out[i] = peek64(cell + 8 + 8 * i)
        }
        ok
    }
