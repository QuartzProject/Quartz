package org.quartzlang.kmp

// quartz.h with every pointer and qz_slot as a Long, so one implementation serves cinterop and JNI.

internal const val QZ_OK = 0
internal const val QZ_TRAP = 2
internal const val LOOKUP_FOUND = 0
internal const val LOOKUP_AMBIGUOUS = 2

/** A type's kind in the high half and its slot count in the low half. */
internal fun typeOf(packed: Long): Type = Type(Kind.of((packed ushr 32).toInt()), packed.toInt())

/** Loads the runtime where that is asynchronous; a no-op where it is linked in. */
internal expect suspend fun loadRuntime()

internal expect fun qzAbiVersion(): Int

/** `qz_compile_with`: the program or 0, and the `qz_diagnostics` (or 0) in `diagnostics[0]`, which the caller frees. */
internal expect fun qzCompileWith(
    names: Array<String>,
    texts: Array<String>,
    emitBinary: Boolean,
    keepLines: Boolean,
    manifest: String?,
    rules: Array<String>,
    levels: IntArray,
    diagnostics: LongArray,
): Long

internal expect fun qzLoad(
    bytes: ByteArray,
    keys: Array<ByteArray>,
    requireSignature: Boolean,
    error: Array<String?>,
): Long

internal expect fun qzProgramBinary(program: Long): ByteArray?

internal expect fun qzProgramFree(program: Long)

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

/** Any bytes as a `Bytes` argument; [qzSandboxStr] takes only valid UTF-8. */
internal expect fun qzSandboxBytes(sandbox: Long, data: ByteArray): Long

/** Any bytes as a `Bytes` result of a host function. */
internal expect fun qzCallBytes(call: Long, data: ByteArray): Long

/** `qz_program_find_import`: the lookup status in the high half and the import index in the low half. */
internal expect fun qzProgramFindImport(program: Long, name: String): Long

internal expect fun qzTypeName(program: Long, type: Int): String

internal expect fun qzApiLevel(): Int

internal expect fun qzDiagnosticsFree(diagnostics: Long)

internal expect fun qzDiagnosticsCount(diagnostics: Long): Int

internal expect fun qzDiagnosticsText(diagnostics: Long): String

internal expect fun qzDiagnosticSeverity(diagnostics: Long, index: Int): Int

internal expect fun qzDiagnosticRule(diagnostics: Long, index: Int): String

internal expect fun qzDiagnosticFile(diagnostics: Long, index: Int): String

internal expect fun qzDiagnosticMessage(diagnostics: Long, index: Int): String

/** Line, column, end line and end column of diagnostic [index], into [out]. */
internal expect fun qzDiagnosticRange(diagnostics: Long, index: Int, out: IntArray)

internal expect fun qzWarningRuleCount(): Int

internal expect fun qzWarningRule(index: Int): String

/** `qz_program_find_function`: the lookup status in the high half and the function index in the low half. */
internal expect fun qzProgramFindFunction(program: Long, name: String): Long

internal expect fun qzExportCount(program: Long): Int

internal expect fun qzExport(program: Long, index: Int): Int

internal expect fun qzFunctionName(program: Long, function: Int): String

internal expect fun qzFunctionPackage(program: Long, function: Int): String

internal expect fun qzFunctionParamName(program: Long, function: Int, index: Int): String

internal expect fun qzFunctionVisibility(program: Long, function: Int): Int

internal expect fun qzFunctionIsSuspend(program: Long, function: Int): Boolean

internal expect fun qzFunctionIsExported(program: Long, function: Int): Boolean

internal expect fun qzImportPackage(program: Long, import: Int): String

internal expect fun qzImportIsRequired(program: Long, import: Int): Boolean

internal expect fun qzTypePackage(program: Long, type: Int): String

internal expect fun qzAttributeCount(program: Long, function: Int): Int

internal expect fun qzAttributeName(program: Long, function: Int, attribute: Int): String

/** The filter function of `@on(..., where = expr)`, or -1. */
internal expect fun qzAttributeFilter(program: Long, function: Int, attribute: Int): Int

internal expect fun qzAttributeArgCount(program: Long, function: Int, attribute: Int): Int

/** Argument [index]: kind, int value and the float's bits into [numbers], key and text into [texts]. */
internal expect fun qzAttributeArg(
    program: Long,
    function: Int,
    attribute: Int,
    index: Int,
    numbers: LongArray,
    texts: Array<String?>,
): Boolean

/** Every slot of the result of a task the last spawn or resume finished, into [out]. */
internal expect fun qzTaskResult(sandbox: Long, task: Int, out: LongArray, slots: Int): Int

/** Text with ill-formed UTF-8 as U+FFFD, as an argument (`qz_sandbox_str_lossy`). */
internal expect fun qzSandboxStrLossy(sandbox: Long, text: ByteArray): Long

/** Text with ill-formed UTF-8 as U+FFFD, as a host function's result (`qz_call_str_lossy`). */
internal expect fun qzCallStrLossy(call: Long, text: ByteArray): Long

/** `qz_deflate`; null only without memory. */
internal expect fun qzDeflate(data: ByteArray, level: Int, framing: Int): ByteArray?

/** `qz_inflate`; null with the reason in `error[0]`. */
internal expect fun qzInflate(data: ByteArray, framing: Int, maxLength: Int, error: Array<String?>): ByteArray?

internal expect fun qzCrc32(crc: Int, data: ByteArray): Int

internal expect fun qzFunctionDoc(program: Long, function: Int): String

internal expect fun qzTypeDoc(program: Long, type: Int): String

internal expect fun qzTypeMemberDoc(program: Long, type: Int, index: Int): String

internal expect fun qzTypeVariantBoxed(program: Long, type: Int, variant: Int, index: Int): Boolean
