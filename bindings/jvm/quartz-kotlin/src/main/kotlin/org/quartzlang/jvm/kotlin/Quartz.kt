package org.quartzlang.jvm.kotlin

import kotlin.reflect.javaType
import kotlin.reflect.typeOf
import org.quartzlang.jvm.CompileOptions
import org.quartzlang.jvm.HandleMapper
import org.quartzlang.jvm.HostCall
import org.quartzlang.jvm.Limits
import org.quartzlang.jvm.Program
import org.quartzlang.jvm.Quartz
import org.quartzlang.jvm.Sandbox
import org.quartzlang.jvm.Source
import org.quartzlang.jvm.Type
import org.quartzlang.jvm.Value
import org.quartzlang.jvm.Values

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

/** Reads the value as [T] like [read], every `@handle` inside turned into its host object by [handles]. */
public inline fun <reified T> Value.read(handles: HandleMapper): T = to(javaTypeOf<T>(), handles) as T

/** Argument [index] of a host call as [T], every `@handle` inside turned into its host object by [handles]. */
public inline fun <reified T> HostCall.argAs(index: Int, handles: HandleMapper): T =
    arg(index, javaTypeOf<T>(), handles) as T

/** [value] (a [Value], `Struct`, `Variant`, `Result`, list, map or scalar) converted to [T]. */
public inline fun <reified T> quartzAs(value: Any?): T = Values.to(value, javaTypeOf<T>()) as T

/** A [HandleMapper] from two functions: the object behind an id, and the id of an object. */
public fun handleMapper(toObject: (id: Long, type: Type) -> Any?, toId: (obj: Any, type: Type) -> Long): HandleMapper =
    object : HandleMapper {
        override fun `object`(id: Long, type: Type): Any? = toObject(id, type)

        override fun id(`object`: Any, type: Type): Long = toId(`object`, type)
    }
