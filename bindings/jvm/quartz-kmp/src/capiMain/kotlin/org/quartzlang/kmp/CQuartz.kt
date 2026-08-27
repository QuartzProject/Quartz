package org.quartzlang.kmp

internal actual suspend fun platformInitialize(): Unit = loadRuntime()

internal actual fun platformAbiVersion(): Int = qzAbiVersion()

internal actual fun platformCompile(sources: List<Source>, options: CompileOptions): Program {
    val diagnostics = arrayOfNulls<String>(1)
    val p = qzCompile(
        sources.map { it.name }.toTypedArray(),
        sources.map { it.text }.toTypedArray(),
        options.emitBinary,
        options.keepLines,
        options.manifest,
        diagnostics,
    )
    if (p == 0L) {
        throw CompileException(diagnostics[0] ?: "")
    }
    return CProgram(p, diagnostics[0] ?: "")
}

internal actual fun platformLoad(bytes: ByteArray, trustedKeys: List<ByteArray>, requireSignature: Boolean): Program {
    trustedKeys.forEach { require(it.size == 32) { "an Ed25519 public key has 32 bytes" } }
    val error = arrayOfNulls<String>(1)
    val p = qzLoad(bytes, trustedKeys.toTypedArray(), requireSignature, error)
    if (p == 0L) {
        throw QuartzException(error[0] ?: "cannot load the module")
    }
    return CProgram(p, "")
}

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

internal class CProgram(address: Long, override val diagnostics: String) : Program {
    val handle = CHandle(address, ::qzProgramFree)
    private val exports = mutableMapOf<String, CExport?>()
    private var closed = false

    val address: Long get() = handle.address.also { check(it != 0L) { "program is closed" } }

    private val types = mutableMapOf<Int, Type>()

    override val imports: List<Import> by lazy {
        val p = address
        List(qzImportCount(p)) { i ->
            val params = List(qzImportArity(p, i)) { k -> type(qzImportParamType(p, i, k)) }
            Import(qzImportName(p, i), Signature(params, type(qzImportResultType(p, i))), qzImportIsNative(p, i))
        }
    }

    /** The type with id [id], its element resolved. */
    fun type(id: Int): Type = types[id] ?: run {
        val packed = typeOf(qzTypeKind(address, id))
        val element = qzTypeElem(address, id)
        Type(packed.kind, packed.slots, id, if (element == 0) null else type(element)).also { types[id] = it }
    }

    override fun binary(): ByteArray? = qzProgramBinary(address)

    override fun find(name: String): Export? = exports.getOrPut(name) {
        val p = address
        val index = qzProgramFunction(p, name)
        if (index == -1) {
            null
        } else {
            val params = List(qzFunctionArity(p, index)) { k -> type(qzFunctionParamType(p, index, k)) }
            CExport(this, name, index, Signature(params, type(qzFunctionResultType(p, index))))
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
) : Export {
    override fun toString(): String = "Export[$name $signature]"
}
