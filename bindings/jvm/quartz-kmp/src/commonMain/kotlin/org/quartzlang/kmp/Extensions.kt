package org.quartzlang.kmp

/** Compiles sources given as `name to text` pairs. */
public fun compileQuartz(vararg sources: Pair<String, String>, options: CompileOptions = CompileOptions()): Program =
    Quartz.compile(sources.map { Source(it.first, it.second) }, options)

/** Runs [block] on a new sandbox of this program and closes it afterwards. */
public inline fun <R> Program.withSandbox(limits: Limits = Limits.DEFAULT, block: (Sandbox) -> R): R =
    Quartz.sandbox(this, limits).use(block)

/** Reads the value as [T]: Long, Int, Double, Float, Boolean, String, ByteArray or Unit. */
public inline fun <reified T> Value.read(): T = when (T::class) {
    Long::class -> asLong()
    Int::class -> asInt()
    Double::class -> asDouble()
    Float::class -> asFloat()
    Boolean::class -> asBoolean()
    String::class -> asString()
    ByteArray::class -> asBytes()
    Unit::class -> Unit
    else -> throw IllegalArgumentException("a Quartz result cannot be read as ${T::class}")
} as T

/** Calls [function] and reads its result as [T], copying a string out before anything else can free it. */
public inline fun <reified T> Sandbox.callAs(function: String, vararg args: Any?): T = call(function, *args).read()
