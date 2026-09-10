package org.quartzlang.jvm.kotlin

import kotlin.reflect.javaType
import kotlin.reflect.typeOf
import org.quartzlang.jvm.CompileOptions
import org.quartzlang.jvm.HostCall
import org.quartzlang.jvm.Limits
import org.quartzlang.jvm.Program
import org.quartzlang.jvm.Quartz
import org.quartzlang.jvm.Sandbox
import org.quartzlang.jvm.Source
import org.quartzlang.jvm.Value

/** Compiles sources given as `name to text` pairs. */
public fun compileQuartz(
    vararg sources: Pair<String, String>,
    options: CompileOptions = CompileOptions.DEFAULT,
): Program = Quartz.compile(sources.map { Source(it.first, it.second) }, options)

/** Runs [block] on a new sandbox of this program and closes it afterwards. */
public inline fun <R> Program.withSandbox(limits: Limits = Limits.DEFAULT, block: (Sandbox) -> R): R =
    Sandbox(this, limits).use(block)

/** The Java type of [T], generic arguments included, for the binding's conversions. */
@OptIn(ExperimentalStdlibApi::class)
@PublishedApi
internal inline fun <reified T> javaTypeOf(): java.lang.reflect.Type = typeOf<T>().javaType

/**
 * Reads the value as [T]: a scalar, String, ByteArray, a data class (by field name), an enum, a sealed class whose
 * subclasses are named like the variants, `List<T>`, `Map<K, V>`, `Result<T, E>`, or Unit.
 */
public inline fun <reified T> Value.read(): T = when (T::class) {
    Long::class -> asLong()
    Int::class -> asInt()
    Double::class -> asDouble()
    Boolean::class -> asBoolean()
    String::class -> asString()
    ByteArray::class -> asBytes()
    Unit::class -> Unit
    else -> to(javaTypeOf<T>())
} as T

/** Calls [function] and reads its result as [T], copying it out before anything else can free it. */
public inline fun <reified T> Sandbox.callAs(function: String, vararg args: Any?): T = call(function, *args).read()

/** Argument [index] of a host call as [T], converted as [read] does. */
public inline fun <reified T> HostCall.argAs(index: Int): T = arg(index, javaTypeOf<T>()) as T
