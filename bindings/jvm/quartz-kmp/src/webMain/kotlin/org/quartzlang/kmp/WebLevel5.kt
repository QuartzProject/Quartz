package org.quartzlang.kmp

// The functions of API level 5 over quartz-capi.wasm: lossy text, DEFLATE and CRC-32, docs and boxed payloads.

private fun doc(address: Int): String = if (address == 0) "" else stringAt(address)

internal actual fun qzSandboxStrLossy(sandbox: Long, text: ByteArray): Long = withBuffer(8) { out ->
    val bytes = bytesIn(text)
    wasm("qz_sandbox_str_lossy_into", p(sandbox), bytes, text.size, out)
    free(bytes)
    peek64(out)
}

internal actual fun qzCallStrLossy(call: Long, text: ByteArray): Long = withBuffer(8) { out ->
    val bytes = bytesIn(text)
    wasm("qz_call_str_lossy_into", p(call), bytes, text.size, out)
    free(bytes)
    peek64(out)
}

internal actual fun qzDeflate(data: ByteArray, level: Int, framing: Int): ByteArray? = withBuffer(4) { length ->
    val bytes = bytesIn(data)
    val out = wasm("qz_deflate", bytes, data.size, level, framing, length)
    free(bytes)
    if (out == 0) null else bytesOut(out, peekI32(length)).also { wasm("qz_free", out) }
}

internal actual fun qzInflate(data: ByteArray, framing: Int, maxLength: Int, error: Array<String?>): ByteArray? =
    withBuffer(8) { cells ->
        pokeI32(cells + 4, 0)
        val bytes = bytesIn(data)
        val out = wasm("qz_inflate", bytes, data.size, framing, maxLength, cells, cells + 4)
        free(bytes)
        error[0] = take(peekI32(cells + 4))
        if (out == 0) null else bytesOut(out, peekI32(cells)).also { wasm("qz_free", out) }
    }

internal actual fun qzCrc32(crc: Int, data: ByteArray): Int {
    val bytes = bytesIn(data)
    try {
        return wasm("qz_crc32", crc, bytes, data.size)
    } finally {
        free(bytes)
    }
}

internal actual fun qzFunctionDoc(program: Long, function: Int): String =
    doc(wasm("qz_program_function_doc", p(program), function))

internal actual fun qzTypeDoc(program: Long, type: Int): String = doc(wasm("qz_type_doc", p(program), type))

internal actual fun qzTypeMemberDoc(program: Long, type: Int, index: Int): String =
    doc(wasm("qz_type_member_doc", p(program), type, index))

internal actual fun qzTypeVariantBoxed(program: Long, type: Int, variant: Int, index: Int): Boolean =
    wasm("qz_type_variant_boxed", p(program), type, variant, index) != 0
