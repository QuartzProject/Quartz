package org.quartzlang.kmp

internal actual suspend fun platformInitialize(): Unit = loadRuntime()

internal actual fun platformAbiVersion(): Int = qzAbiVersion()

internal actual fun platformWarningRules(): List<String> = List(qzWarningRuleCount()) { qzWarningRule(it) }

internal actual fun platformCompile(sources: List<Source>, options: CompileOptions): Program {
    check(qzApiLevel() >= API_LEVEL_NEEDED) { "the Quartz runtime is older than API level $API_LEVEL_NEEDED" }
    val out = LongArray(1)
    val p = qzCompileWith(
        sources.map { it.name }.toTypedArray(),
        sources.map { it.text }.toTypedArray(),
        options.emitBinary,
        options.keepLines,
        options.manifest,
        options.rules.keys.toTypedArray(),
        options.rules.values.map { it.ordinal }.toIntArray(),
        out,
    )
    val d = out[0]
    val text: String
    val diagnostics: List<Diagnostic>
    try {
        text = if (d == 0L) "" else qzDiagnosticsText(d)
        diagnostics = if (d == 0L) emptyList() else readDiagnostics(d)
    } finally {
        if (d != 0L) {
            qzDiagnosticsFree(d)
        }
    }
    if (p == 0L) {
        throw CompileException(text, diagnostics)
    }
    return CProgram(p, text, diagnostics)
}

private const val API_LEVEL_NEEDED = 5

private fun readDiagnostics(d: Long): List<Diagnostic> {
    val range = IntArray(4)
    return List(qzDiagnosticsCount(d)) { i ->
        qzDiagnosticRange(d, i, range)
        Diagnostic(
            Diagnostic.Severity.of(qzDiagnosticSeverity(d, i)), qzDiagnosticRule(d, i), qzDiagnosticFile(d, i),
            range[0], range[1], range[2], range[3], qzDiagnosticMessage(d, i),
        )
    }
}

internal actual fun platformLoad(bytes: ByteArray, trustedKeys: List<ByteArray>, requireSignature: Boolean): Program {
    trustedKeys.forEach { require(it.size == 32) { "an Ed25519 public key has 32 bytes" } }
    val error = arrayOfNulls<String>(1)
    val p = qzLoad(bytes, trustedKeys.toTypedArray(), requireSignature, error)
    if (p == 0L) {
        throw QuartzException(error[0] ?: "cannot load the module")
    }
    return CProgram(p, "", emptyList())
}

internal actual fun platformDeflate(data: ByteArray, level: Int, framing: Framing): ByteArray =
    qzDeflate(data, level, framing.ordinal) ?: throw QuartzException("out of memory compressing")

internal actual fun platformInflate(data: ByteArray, framing: Framing, maxLength: Int): ByteArray {
    val error = arrayOfNulls<String>(1)
    return qzInflate(data, framing.ordinal, maxLength, error)
        ?: throw QuartzException(error[0] ?: "cannot decompress the data")
}

internal actual fun platformCrc32(crc: Long, data: ByteArray): Long =
    qzCrc32(crc.toInt(), data).toLong() and 0xFFFF_FFFFL

internal actual fun platformSandbox(program: Program, limits: Limits): Sandbox {
    val p = program as? CProgram ?: throw IllegalArgumentException("a program of another runtime")
    return CSandbox(p, limits)
}

/** A native object freed once its owner closed it and everything depending on it let go. */
internal class CHandle(address: Long, private val free: (Long) -> Unit) {
    private var refs = 1
    private val dependencies = mutableListOf<CHandle>()
    private val afterFree = mutableListOf<() -> Unit>()
    var address: Long = address
        private set

    fun retain() {
        check(refs > 0) { "native object already freed" }
        refs++
    }

    fun dependOn(other: CHandle) {
        other.retain()
        dependencies += other
    }

    fun afterFree(action: () -> Unit) {
        afterFree += action
    }

    fun release() {
        if (refs == 0 || --refs != 0) {
            return
        }
        free(address)
        address = 0
        afterFree.forEach { it() }
        dependencies.forEach { it.release() }
    }
}

internal class CProgram(
    address: Long,
    override val diagnosticsText: String,
    override val diagnostics: List<Diagnostic>,
) : Program {
    val handle = CHandle(address, ::qzProgramFree)
    private val byName = mutableMapOf<String, CExport?>()
    private val functions = mutableMapOf<Int, CExport>()
    private var closed = false

    val address: Long get() = handle.address.also { check(it != 0L) { "program is closed" } }

    private val types = mutableMapOf<Int, Type>()

    override val imports: List<Import> by lazy {
        val p = address
        List(qzImportCount(p)) { i ->
            val params = List(qzImportArity(p, i)) { k -> type(qzImportParamType(p, i, k)) }
            Import(
                qzImportName(p, i), Signature(params, type(qzImportResultType(p, i))), qzImportIsNative(p, i),
                qzImportPackage(p, i), qzImportIsRequired(p, i),
            )
        }
    }

    /** The type with id [id], its element, key, fields and variants resolved; recursive types terminate. */
    fun type(id: Int): Type = types[id] ?: run {
        val p = address
        val packed = typeOf(qzTypeKind(p, id))
        val t = Type(packed.kind, packed.slots, id)
        types[id] = t
        val elementId = qzTypeElem(p, id)
        val element = if (elementId == 0) null else type(elementId)
        var key: Type? = null
        var fields = emptyList<Type.Field>()
        var cases = emptyList<Type.Case>()
        var pointer = false
        when (t.kind) {
            Kind.MAP -> key = qzTypeKey(p, id).takeIf { it != 0 }?.let(::type)
            Kind.STRUCT, Kind.TUPLE -> fields = List(qzTypeFieldCount(p, id)) { i ->
                val f = qzTypeField(p, id, i)
                val name = if (t.kind == Kind.STRUCT) qzTypeFieldName(p, id, i) else ""
                Type.Field(name, type((f ushr 32).toInt()), f.toInt(), doc = qzTypeMemberDoc(p, id, i))
            }
            Kind.ENUM -> cases = List(qzTypeVariantCount(p, id)) { v ->
                val payload = List(qzTypeVariantPayloadCount(p, id, v)) { i ->
                    val f = qzTypeVariantPayload(p, id, v, i)
                    val valueId = (f ushr 32).toInt()
                    if (qzTypeVariantBoxed(p, id, v, i)) {
                        Type.Field("", type(qzTypeElem(p, valueId)), f.toInt(), box = type(valueId))
                    } else {
                        Type.Field("", type(valueId), f.toInt())
                    }
                }
                Type.Case(qzTypeVariantName(p, id, v), v, payload, qzTypeMemberDoc(p, id, v))
            }
            Kind.NULLABLE -> pointer = qzTypeNullableIsPointer(p, id)
            else -> {}
        }
        t.name = qzTypeName(p, id)
        t.packageName = qzTypePackage(p, id)
        t.doc = qzTypeDoc(p, id)
        t.complete(element, key, fields, cases, pointer)
        t
    }

    override fun binary(): ByteArray? = qzProgramBinary(address)

    override fun findImport(name: String): Import? {
        val found = qzProgramFindImport(address, name)
        return when ((found ushr 32).toInt()) {
            LOOKUP_FOUND -> imports.getOrNull(found.toInt())
            LOOKUP_AMBIGUOUS -> throw IllegalArgumentException("several imports are named $name; give its package")
            else -> null
        }
    }

    override val exports: List<Export> by lazy {
        val p = address
        List(qzExportCount(p)) { function(qzExport(p, it)) }
    }

    override fun find(name: String): Export? = byName.getOrPut(name) {
        val found = qzProgramFindFunction(address, name)
        when ((found ushr 32).toInt()) {
            LOOKUP_FOUND -> function(found.toInt())
            LOOKUP_AMBIGUOUS -> throw IllegalArgumentException("several functions are named $name; give its package")
            else -> null
        }
    }

    /** Function [index] with its signature, names and flags. */
    fun function(index: Int): CExport = functions.getOrPut(index) {
        val p = address
        val arity = qzFunctionArity(p, index)
        val params = List(arity) { k -> type(qzFunctionParamType(p, index, k)) }
        CExport(
            this, qzFunctionName(p, index), index, Signature(params, type(qzFunctionResultType(p, index))),
            qzFunctionPackage(p, index), List(arity) { qzFunctionParamName(p, index, it) },
            Visibility.of(qzFunctionVisibility(p, index)), qzFunctionIsSuspend(p, index),
            qzFunctionIsExported(p, index), qzFunctionDoc(p, index),
        )
    }

    /** The attributes of function [index], read when first asked for. */
    fun attributesOf(index: Int): List<Attribute> {
        val p = address
        val numbers = LongArray(3)
        val texts = arrayOfNulls<String>(2)
        return List(qzAttributeCount(p, index)) { a ->
            val args = (0 until qzAttributeArgCount(p, index, a)).mapNotNull { i ->
                if (!qzAttributeArg(p, index, a, i, numbers, texts)) {
                    return@mapNotNull null
                }
                val kind = Attribute.ArgumentKind.of(numbers[0].toInt())
                val value: Any = when (kind) {
                    Attribute.ArgumentKind.INT -> numbers[1]
                    Attribute.ArgumentKind.FLOAT -> Double.fromBits(numbers[2])
                    Attribute.ArgumentKind.BOOL -> numbers[1] != 0L
                    else -> texts[1] ?: ""
                }
                Attribute.Argument(texts[0] ?: "", kind, value)
            }
            val filter = qzAttributeFilter(p, index, a)
            Attribute(qzAttributeName(p, index, a), args, if (filter == -1) null else function(filter))
        }
    }

    override fun packageInterface(manifest: String?): String {
        val error = arrayOfNulls<String>(1)
        return qzProgramInterface(address, manifest, error)
            ?: throw QuartzException(error[0] ?: "the program has no package interface")
    }

    override fun close() {
        if (!closed) {
            closed = true
            handle.release()
        }
    }
}

internal class CExport(
    val program: CProgram,
    override val name: String,
    val index: Int,
    override val signature: Signature,
    override val packageName: String,
    override val parameterNames: List<String>,
    override val visibility: Visibility,
    override val isSuspend: Boolean,
    override val isExported: Boolean,
    override val doc: String,
) : Export {
    override val attributes: List<Attribute> by lazy { program.attributesOf(index) }

    /** First argument slot of each parameter, and how many slots a call passes. */
    val offsets: IntArray = IntArray(signature.arity).also { o ->
        var at = 0
        signature.params.forEachIndexed { i, t ->
            o[i] = at
            at += t.slots
        }
    }
    val argSlots: Int = signature.params.sumOf { it.slots }

    /** Every parameter is one slot that is not laid out field by field. */
    val simple: Boolean = signature.params.all { it.slots == 1 && !it.isComposite }

    override fun toString(): String = "Export[$name $signature]"
}
