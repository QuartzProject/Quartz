@file:JvmName("QuartzJni")

package org.quartzlang.kmp

// JNI over quartz.h (src/androidMain/c/quartz_jni.c): ART has no java.lang.foreign.

@Suppress("unused")
private val loaded = System.loadLibrary("quartz_jni")

internal actual suspend fun loadRuntime() {}

internal actual external fun qzAbiVersion(): Int

internal actual external fun qzCompile(
    names: Array<String>,
    texts: Array<String>,
    emitBinary: Boolean,
    keepLines: Boolean,
    manifest: String?,
    diagnostics: Array<String?>,
): Long

internal actual external fun qzLoad(
    bytes: ByteArray,
    keys: Array<ByteArray>,
    requireSignature: Boolean,
    error: Array<String?>,
): Long

internal actual external fun qzProgramBinary(program: Long): ByteArray?

internal actual external fun qzProgramFree(program: Long)

internal actual external fun qzProgramFunction(program: Long, name: String): Int

internal actual external fun qzFunctionArity(program: Long, function: Int): Int

internal actual external fun qzFunctionParamType(program: Long, function: Int, index: Int): Int

internal actual external fun qzFunctionResultType(program: Long, function: Int): Int

internal actual external fun qzImportCount(program: Long): Int

internal actual external fun qzImportName(program: Long, import: Int): String

internal actual external fun qzImportArity(program: Long, import: Int): Int

internal actual external fun qzImportParamType(program: Long, import: Int, index: Int): Int

internal actual external fun qzImportResultType(program: Long, import: Int): Int

internal actual external fun qzImportIsNative(program: Long, import: Int): Boolean

internal actual external fun qzTypeKind(program: Long, type: Int): Long

internal actual external fun qzTypeElem(program: Long, type: Int): Int

internal actual external fun qzProgramInterface(program: Long, manifest: String?, error: Array<String?>): String?

internal actual external fun qzSandboxNew(program: Long, memory: Long, fuel: Long, depth: Int, tier: Int): Long

internal actual external fun qzSandboxFree(sandbox: Long)

internal actual external fun qzBind(sandbox: Long, name: String, binding: CBinding): Long

internal actual external fun qzUnbind(token: Long)

internal actual external fun qzStart(sandbox: Long): Int

internal actual external fun qzCall(sandbox: Long, function: Int, args: LongArray, count: Int, result: LongArray): Int

internal actual external fun qzCallWide(
    sandbox: Long,
    function: Int,
    args: LongArray,
    count: Int,
    result: LongArray,
    slots: Int,
): Int

internal actual external fun qzSpawn(sandbox: Long, function: Int, args: LongArray, count: Int, outcome: LongArray): Int

internal actual external fun qzResume(sandbox: Long, task: Int, value: Long, outcome: LongArray): Int

internal actual external fun qzCancel(sandbox: Long, task: Int)

internal actual external fun qzError(sandbox: Long): String

internal actual external fun qzTrapCode(sandbox: Long): Int

internal actual external fun qzSandboxStr(sandbox: Long, utf8: ByteArray): Long

internal actual external fun qzSetFuel(sandbox: Long, fuel: Long)

internal actual external fun qzFuelLeft(sandbox: Long): Long

internal actual external fun qzFuelPerCall(sandbox: Long): Long

internal actual external fun qzMemoryUsed(sandbox: Long): Long

internal actual external fun qzLiveTasks(sandbox: Long): Long

internal actual external fun qzExportState(sandbox: Long): String?

internal actual external fun qzImportState(sandbox: Long, json: String): Int

internal actual external fun qzLink(client: Long, pkg: String, provider: Long): Int

internal actual external fun qzReload(sandbox: Long, program: Long): Int

internal actual external fun qzExportHandle(sandbox: Long, handle: Long): String?

internal actual external fun qzImportHandle(sandbox: Long, handle: Long, json: String): Int

internal actual external fun qzDropHandle(sandbox: Long, handle: Long)

internal actual external fun qzStr(slot: Long): ByteArray

internal actual external fun qzSlot(args: Long, index: Int): Long

internal actual external fun qzCallStr(call: Long, utf8: ByteArray): Long

internal actual external fun qzSetSlot(address: Long, index: Int, bits: Long)

internal actual external fun qzCallCharge(call: Long, units: Long): Boolean

internal actual external fun qzCallFail(call: Long, message: String)

internal actual external fun qzWaitType(sandbox: Long, task: Int): Int

internal actual external fun qzSandboxList(sandbox: Long, type: Int): Long

internal actual external fun qzListPush(sandbox: Long, list: Long, element: LongArray, slots: Int): Boolean

internal actual external fun qzListLength(list: Long): Long

internal actual external fun qzListGet(list: Long, index: Long, out: LongArray, slots: Int): Boolean

internal actual external fun qzTypeFieldCount(program: Long, type: Int): Int

internal actual external fun qzTypeField(program: Long, type: Int, index: Int): Long

internal actual external fun qzTypeFieldName(program: Long, type: Int, index: Int): String

internal actual external fun qzTypeVariantCount(program: Long, type: Int): Int

internal actual external fun qzTypeVariantName(program: Long, type: Int, variant: Int): String

internal actual external fun qzTypeVariantPayloadCount(program: Long, type: Int, variant: Int): Int

internal actual external fun qzTypeVariantPayload(program: Long, type: Int, variant: Int, index: Int): Long

internal actual external fun qzTypeNullableIsPointer(program: Long, type: Int): Boolean

internal actual external fun qzTypeKey(program: Long, type: Int): Int

internal actual external fun qzResumeWide(
    sandbox: Long,
    task: Int,
    value: LongArray,
    slots: Int,
    outcome: LongArray,
): Int

internal actual external fun qzSandboxMap(sandbox: Long, type: Int): Long

internal actual external fun qzMapSet(sandbox: Long, map: Long, key: LongArray, value: LongArray): Boolean

internal actual external fun qzMapLength(map: Long): Long

internal actual external fun qzMapEntry(
    sandbox: Long,
    map: Long,
    index: Long,
    key: LongArray,
    value: LongArray,
): Boolean

internal actual external fun qzSandboxBytes(sandbox: Long, data: ByteArray): Long

internal actual external fun qzCallBytes(call: Long, data: ByteArray): Long

internal actual external fun qzProgramFindImport(program: Long, name: String): Long

internal actual external fun qzTypeName(program: Long, type: Int): String
