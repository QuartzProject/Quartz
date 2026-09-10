package org.quartzlang.kmp

// quartz.h with every pointer and qz_slot as a Long, so one implementation serves cinterop and JNI.

internal const val QZ_OK = 0
internal const val QZ_TRAP = 2

/** A type's kind in the high half and its slot count in the low half. */
internal fun typeOf(packed: Long): Type = Type(Kind.of((packed ushr 32).toInt()), packed.toInt())

/** Loads the runtime where that is asynchronous; a no-op where it is linked in. */
internal expect suspend fun loadRuntime()

internal expect fun qzAbiVersion(): Int

/** Returns the program, or 0 with the reason in `diagnostics[0]` (also set on success, for warnings). */
internal expect fun qzCompile(
    names: Array<String>,
    texts: Array<String>,
    emitBinary: Boolean,
    keepLines: Boolean,
    manifest: String?,
    diagnostics: Array<String?>,
): Long

internal expect fun qzLoad(
    bytes: ByteArray,
    keys: Array<ByteArray>,
    requireSignature: Boolean,
    error: Array<String?>,
): Long

internal expect fun qzProgramBinary(program: Long): ByteArray?

internal expect fun qzProgramFree(program: Long)

internal expect fun qzProgramFunction(program: Long, name: String): Int

internal expect fun qzFunctionArity(program: Long, function: Int): Int

internal expect fun qzFunctionParamType(program: Long, function: Int, index: Int): Int

internal expect fun qzFunctionResultType(program: Long, function: Int): Int

internal expect fun qzImportCount(program: Long): Int

internal expect fun qzImportName(program: Long, import: Int): String

internal expect fun qzImportArity(program: Long, import: Int): Int

internal expect fun qzImportParamType(program: Long, import: Int, index: Int): Int

internal expect fun qzImportResultType(program: Long, import: Int): Int

internal expect fun qzImportIsNative(program: Long, import: Int): Boolean

/** Kind and slot count of a type id, packed as [typeOf] reads them. */
internal expect fun qzTypeKind(program: Long, type: Int): Long

internal expect fun qzTypeElem(program: Long, type: Int): Int

internal expect fun qzProgramInterface(program: Long, manifest: String?, error: Array<String?>): String?

internal expect fun qzSandboxNew(program: Long, memory: Long, fuel: Long, depth: Int, tier: Int): Long

internal expect fun qzSandboxFree(sandbox: Long)

/** Binds a direct host function that calls [CBinding.dispatch]; returns a token for [qzUnbind], or 0. */
internal expect fun qzBind(sandbox: Long, name: String, binding: CBinding): Long

/** Releases what [qzBind] kept, once the sandbox is freed. */
internal expect fun qzUnbind(token: Long)

internal expect fun qzStart(sandbox: Long): Int

internal expect fun qzCall(sandbox: Long, function: Int, args: LongArray, count: Int, result: LongArray): Int

/** A call whose result takes `slots` slots, all copied into `result`. */
internal expect fun qzCallWide(
    sandbox: Long,
    function: Int,
    args: LongArray,
    count: Int,
    result: LongArray,
    slots: Int,
): Int

/** `outcome` gets kind, task and value. */
internal expect fun qzSpawn(sandbox: Long, function: Int, args: LongArray, count: Int, outcome: LongArray): Int

internal expect fun qzResume(sandbox: Long, task: Int, value: Long, outcome: LongArray): Int

internal expect fun qzCancel(sandbox: Long, task: Int)

internal expect fun qzError(sandbox: Long): String

internal expect fun qzTrapCode(sandbox: Long): Int

internal expect fun qzSandboxStr(sandbox: Long, utf8: ByteArray): Long

internal expect fun qzSetFuel(sandbox: Long, fuel: Long)

internal expect fun qzFuelLeft(sandbox: Long): Long

internal expect fun qzFuelPerCall(sandbox: Long): Long

internal expect fun qzMemoryUsed(sandbox: Long): Long

internal expect fun qzLiveTasks(sandbox: Long): Long

internal expect fun qzExportState(sandbox: Long): String?

internal expect fun qzImportState(sandbox: Long, json: String): Int

internal expect fun qzLink(client: Long, pkg: String, provider: Long): Int

internal expect fun qzReload(sandbox: Long, program: Long): Int

internal expect fun qzExportHandle(sandbox: Long, handle: Long): String?

internal expect fun qzImportHandle(sandbox: Long, handle: Long, json: String): Int

internal expect fun qzDropHandle(sandbox: Long, handle: Long)

internal expect fun qzStr(slot: Long): ByteArray

internal expect fun qzSlot(args: Long, index: Int): Long

internal expect fun qzCallStr(call: Long, utf8: ByteArray): Long

/** Writes slot `index` of the slots at [address] (a host function's result). */
internal expect fun qzSetSlot(address: Long, index: Int, bits: Long)

internal expect fun qzCallCharge(call: Long, units: Long): Boolean

internal expect fun qzCallFail(call: Long, message: String)

internal expect fun qzWaitType(sandbox: Long, task: Int): Int

internal expect fun qzSandboxList(sandbox: Long, type: Int): Long

/** Appends an element given as its `slots` slots. */
internal expect fun qzListPush(sandbox: Long, list: Long, element: LongArray, slots: Int): Boolean

internal expect fun qzListLength(list: Long): Long

/** Element [index] of a list, its `slots` slots in `out`; false out of range or with the wrong slot count. */
internal expect fun qzListGet(list: Long, index: Long, out: LongArray, slots: Int): Boolean

internal expect fun qzTypeFieldCount(program: Long, type: Int): Int

/** Type id of field or element [index] in the high half and its slot offset in the low half. */
internal expect fun qzTypeField(program: Long, type: Int, index: Int): Long

internal expect fun qzTypeFieldName(program: Long, type: Int, index: Int): String

internal expect fun qzTypeVariantCount(program: Long, type: Int): Int

internal expect fun qzTypeVariantName(program: Long, type: Int, variant: Int): String

internal expect fun qzTypeVariantPayloadCount(program: Long, type: Int, variant: Int): Int

/** Type id of payload value [index] in the high half and its slot offset in the low half. */
internal expect fun qzTypeVariantPayload(program: Long, type: Int, variant: Int, index: Int): Long

internal expect fun qzTypeNullableIsPointer(program: Long, type: Int): Boolean

internal expect fun qzTypeKey(program: Long, type: Int): Int

/** Resumes a task with a value of `slots` slots. */
internal expect fun qzResumeWide(sandbox: Long, task: Int, value: LongArray, slots: Int, outcome: LongArray): Int

internal expect fun qzSandboxMap(sandbox: Long, type: Int): Long

/** Inserts or replaces an entry; [key] and [value] hold as many slots as the map's types take. */
internal expect fun qzMapSet(sandbox: Long, map: Long, key: LongArray, value: LongArray): Boolean

internal expect fun qzMapLength(map: Long): Long

/** Entry [index] in insertion order, copied into [key] and [value]; false out of range. */
internal expect fun qzMapEntry(sandbox: Long, map: Long, index: Long, key: LongArray, value: LongArray): Boolean
