@file:OptIn(ExperimentalWasmJsInterop::class)

package org.quartzlang.kmp

import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException
import kotlin.coroutines.suspendCoroutine
import kotlin.js.ExperimentalWasmJsInterop
import kotlin.js.Promise

private var module: JsAny? = null

private fun importCapi(): Promise<JsAny> = js("import('quartz-capi').then((m) => (m.default ?? m)({}))")

private fun callExport(m: JsAny, name: String, a: Int, b: Int, c: Int, d: Int, e: Int, f: Int, g: Int): Int =
    js("m['_' + name](a, b, c, d, e, f, g) | 0")

private fun heapU8(m: JsAny, address: Int): Int = js("m.HEAPU8[address >>> 0]")

private fun setHeapU8(m: JsAny, address: Int, value: Int): Unit = js("{ m.HEAPU8[address >>> 0] = value; }")

private fun heapI32(m: JsAny, address: Int): Int = js("m.HEAP32[address >>> 2]")

private fun setHeapI32(m: JsAny, address: Int, value: Int): Unit = js("{ m.HEAP32[address >>> 2] = value; }")

private fun addFunction(m: JsAny, f: (Int, Int, Int, Int) -> Int): Int = js("m.addFunction(f, 'iiiii')")

private fun failure(error: JsAny?): Throwable = IllegalStateException("cannot load quartz-capi: $error")

internal actual suspend fun loadModule() {
    if (module != null) {
        return
    }
    val promise = importCapi()
    module = suspendCoroutine { cont ->
        promise.then({ cont.resume(it); null }, { cont.resumeWithException(failure(it)); null })
    }
}

private fun loaded(): JsAny = module ?: throw IllegalStateException("call Quartz.initialize() first")

internal actual fun wasm(name: String, a: Int, b: Int, c: Int, d: Int, e: Int, f: Int, g: Int): Int =
    callExport(loaded(), name, a, b, c, d, e, f, g)

internal actual fun peekU8(address: Int): Int = heapU8(loaded(), address)

internal actual fun pokeU8(address: Int, value: Int) = setHeapU8(loaded(), address, value and 0xFF)

internal actual fun peekI32(address: Int): Int = heapI32(loaded(), address)

internal actual fun pokeI32(address: Int, value: Int) = setHeapI32(loaded(), address, value)

internal actual fun addDispatch(): Int = addFunction(loaded()) { call, args, result, user ->
    webDispatch(call, args, result, user)
}
