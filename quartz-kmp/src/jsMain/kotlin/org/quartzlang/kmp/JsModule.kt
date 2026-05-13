package org.quartzlang.kmp

import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException
import kotlin.coroutines.suspendCoroutine
import kotlin.js.Promise

private var module: dynamic = null

internal actual suspend fun loadModule() {
    if (module != null) {
        return
    }
    val create: dynamic = js("require('quartz-capi')")
    val promise = create(js("({})")).unsafeCast<Promise<dynamic>>()
    module = suspendCoroutine<dynamic> { cont ->
        promise.then({ value: dynamic -> cont.resume(value) }, { error: Throwable -> cont.resumeWithException(error) })
    }
}

private fun loaded(): dynamic = module ?: throw IllegalStateException("call Quartz.initialize() first")

internal actual fun wasm(name: String, a: Int, b: Int, c: Int, d: Int, e: Int, f: Int, g: Int): Int {
    val r: dynamic = loaded()["_$name"](a, b, c, d, e, f, g)
    return if (r == null) 0 else r.unsafeCast<Number>().toInt()
}

private fun unsigned(address: Int): Double = (address.toLong() and 0xFFFF_FFFFL).toDouble()

internal actual fun peekU8(address: Int): Int = loaded().HEAPU8[unsigned(address)].unsafeCast<Int>()

internal actual fun pokeU8(address: Int, value: Int) {
    loaded().HEAPU8[unsigned(address)] = value and 0xFF
}

internal actual fun peekI32(address: Int): Int = loaded().HEAP32[address ushr 2].unsafeCast<Int>()

internal actual fun pokeI32(address: Int, value: Int) {
    loaded().HEAP32[address ushr 2] = value
}

internal actual fun addDispatch(): Int {
    val trampoline = { call: Int, args: Int, result: Int, user: Int -> webDispatch(call, args, result, user) }
    return loaded().addFunction(trampoline, "iiiii").unsafeCast<Int>()
}
