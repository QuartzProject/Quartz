@file:OptIn(ExperimentalForeignApi::class)

package org.quartzlang.kmp

import cnames.structs.qz_call
import cnames.structs.qz_program
import cnames.structs.qz_sandbox
import kotlinx.cinterop.ByteVar
import kotlinx.cinterop.CPointer
import kotlinx.cinterop.CPointerVar
import kotlinx.cinterop.ExperimentalForeignApi
import kotlinx.cinterop.UByteVar
import kotlinx.cinterop.ULongVar
import kotlinx.cinterop.addressOf
import kotlinx.cinterop.alloc
import kotlinx.cinterop.memScoped
import kotlinx.cinterop.ptr
import kotlinx.cinterop.readBytes
import kotlinx.cinterop.reinterpret
import kotlinx.cinterop.toCPointer
import kotlinx.cinterop.toKString
import kotlinx.cinterop.usePinned
import kotlinx.cinterop.value
import org.quartzlang.kmp.cinterop.qz_crc32
import org.quartzlang.kmp.cinterop.qz_deflate
import org.quartzlang.kmp.cinterop.qz_framing
import org.quartzlang.kmp.cinterop.qz_free
import org.quartzlang.kmp.cinterop.qz_inflate
import org.quartzlang.kmp.cinterop.qz_program_function_doc
import org.quartzlang.kmp.cinterop.qz_type_doc
import org.quartzlang.kmp.cinterop.qz_type_member_doc
import org.quartzlang.kmp.cinterop.qz_type_variant_boxed
import org.quartzlang.kmp.cinterop.qzk_call_str_lossy
import org.quartzlang.kmp.cinterop.qzk_sandbox_str_lossy

// The functions of API level 5: lossy text, DEFLATE and CRC-32, docs and boxed recursive payloads.

private fun prog5(p: Long): CPointer<qz_program> = p.toCPointer()!!

private fun doc(s: CPointer<ByteVar>?): String = s?.toKString() ?: ""

/** Runs [use] with a pointer to [data] (a valid one even when it is empty) and its length. */
private inline fun <R> pinned(data: ByteArray, use: (CPointer<UByteVar>, ULong) -> R): R = if (data.isEmpty()) {
    memScoped { use(alloc<UByteVar>().ptr, 0u) }
} else {
    data.usePinned { use(it.addressOf(0).reinterpret(), data.size.toULong()) }
}

internal actual fun qzSandboxStrLossy(sandbox: Long, text: ByteArray): Long = pinned(text) { data, length ->
    qzk_sandbox_str_lossy(sandbox.toCPointer<qz_sandbox>(), data.reinterpret(), length).toLong()
}

internal actual fun qzCallStrLossy(call: Long, text: ByteArray): Long = pinned(text) { data, length ->
    qzk_call_str_lossy(call.toCPointer<qz_call>(), data.reinterpret(), length).toLong()
}

private fun framing(code: Int): qz_framing = code.toUInt()

private fun taken(bytes: CPointer<UByteVar>, length: ULong): ByteArray = try {
    bytes.readBytes(length.toInt())
} finally {
    qz_free(bytes)
}

internal actual fun qzDeflate(data: ByteArray, level: Int, framing: Int): ByteArray? = memScoped {
    val length = alloc<ULongVar>()
    val out = pinned(data) { d, n -> qz_deflate(d, n, level, framing(framing), length.ptr) } ?: return@memScoped null
    taken(out, length.value)
}

internal actual fun qzInflate(data: ByteArray, framing: Int, maxLength: Int, error: Array<String?>): ByteArray? =
    memScoped {
        val length = alloc<ULongVar>()
        val message = alloc<CPointerVar<ByteVar>>()
        val out = pinned(data) { d, n ->
            qz_inflate(d, n, framing(framing), maxLength.toULong(), length.ptr, message.ptr)
        }
        message.value?.let {
            error[0] = it.toKString()
            qz_free(it)
        }
        out?.let { taken(it, length.value) }
    }

internal actual fun qzCrc32(crc: Int, data: ByteArray): Int =
    pinned(data) { d, n -> qz_crc32(crc.toUInt(), d, n).toInt() }

internal actual fun qzFunctionDoc(program: Long, function: Int): String =
    doc(qz_program_function_doc(prog5(program), function.toUInt()))

internal actual fun qzTypeDoc(program: Long, type: Int): String = doc(qz_type_doc(prog5(program), type.toUInt()))

internal actual fun qzTypeMemberDoc(program: Long, type: Int, index: Int): String =
    doc(qz_type_member_doc(prog5(program), type.toUInt(), index.toUInt()))

internal actual fun qzTypeVariantBoxed(program: Long, type: Int, variant: Int, index: Int): Boolean =
    qz_type_variant_boxed(prog5(program), type.toUInt(), variant.toUInt(), index.toUInt())
