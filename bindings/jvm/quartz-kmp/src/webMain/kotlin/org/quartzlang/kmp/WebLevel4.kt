package org.quartzlang.kmp

// The functions of API level 4 over quartz-capi.wasm; structs are laid out as wasm32 does (4-byte pointers).

private fun text(address: Int): String = if (address == 0) "" else stringAt(address)

private fun status(lookup: Int, index: Int): Long = (lookup.toLong() shl 32) or (index.toLong() and 0xFFFF_FFFFL)

internal actual fun qzApiLevel(): Int = wasm("qz_api_level")

internal actual fun qzCompileWith(
    names: Array<String>,
    texts: Array<String>,
    emitBinary: Boolean,
    keepLines: Boolean,
    manifest: String?,
    rules: Array<String>,
    levels: IntArray,
    diagnostics: LongArray,
): Long {
    val owned = mutableListOf<Int>()
    try {
        val sources = malloc(12 * names.size).also { owned += it }
        for (i in names.indices) {
            val bytes = texts[i].encodeToByteArray()
            pokeI32(sources + 12 * i, cString(names[i]).also { owned += it })
            pokeI32(sources + 12 * i + 4, bytesIn(bytes).also { owned += it })
            pokeI32(sources + 12 * i + 8, bytes.size)
        }
        val options = malloc(8).also { owned += it }
        pokeI32(options, (if (emitBinary) 1 else 0) or (if (keepLines) 0x100 else 0))
        pokeI32(options + 4, if (manifest == null) 0 else cString(manifest).also { owned += it })
        val table = malloc(8 * rules.size).also { owned += it }
        for (i in rules.indices) {
            pokeI32(table + 8 * i, cString(rules[i]).also { owned += it })
            pokeI32(table + 8 * i + 4, levels[i])
        }
        val out = malloc(4).also { owned += it }
        pokeI32(out, 0)
        val program = wasm("qz_compile_with", sources, names.size, options, table, rules.size, out)
        diagnostics[0] = addr(peekI32(out))
        return addr(program)
    } finally {
        owned.forEach(::free)
    }
}

internal actual fun qzDiagnosticsFree(diagnostics: Long) {
    wasm("qz_diagnostics_free", p(diagnostics))
}

internal actual fun qzDiagnosticsCount(diagnostics: Long): Int = wasm("qz_diagnostics_count", p(diagnostics))

internal actual fun qzDiagnosticsText(diagnostics: Long): String = text(wasm("qz_diagnostics_text", p(diagnostics)))

internal actual fun qzDiagnosticSeverity(diagnostics: Long, index: Int): Int =
    wasm("qz_diagnostic_severity", p(diagnostics), index)

internal actual fun qzDiagnosticRule(diagnostics: Long, index: Int): String =
    text(wasm("qz_diagnostic_rule", p(diagnostics), index))

internal actual fun qzDiagnosticFile(diagnostics: Long, index: Int): String =
    text(wasm("qz_diagnostic_file", p(diagnostics), index))

internal actual fun qzDiagnosticMessage(diagnostics: Long, index: Int): String =
    text(wasm("qz_diagnostic_message", p(diagnostics), index))

internal actual fun qzDiagnosticRange(diagnostics: Long, index: Int, out: IntArray) = withBuffer(16) { cells ->
    wasm("qz_diagnostic_range", p(diagnostics), index, cells, cells + 4, cells + 8, cells + 12)
    for (i in 0 until 4) {
        out[i] = peekI32(cells + 4 * i)
    }
}

internal actual fun qzWarningRuleCount(): Int = wasm("qz_warning_rule_count")

internal actual fun qzWarningRule(index: Int): String = text(wasm("qz_warning_rule", index))

internal actual fun qzProgramFindFunction(program: Long, name: String): Long = withText(name) { n, _ ->
    withBuffer(4) { index ->
        pokeI32(index, 0)
        status(wasm("qz_program_find_function", p(program), n, index), peekI32(index))
    }
}

internal actual fun qzExportCount(program: Long): Int = wasm("qz_program_export_count", p(program))

internal actual fun qzExport(program: Long, index: Int): Int = wasm("qz_program_export", p(program), index)

internal actual fun qzFunctionName(program: Long, function: Int): String =
    text(wasm("qz_program_function_name", p(program), function))

internal actual fun qzFunctionPackage(program: Long, function: Int): String =
    text(wasm("qz_program_function_package", p(program), function))

internal actual fun qzFunctionParamName(program: Long, function: Int, index: Int): String =
    text(wasm("qz_program_function_param_name", p(program), function, index))

internal actual fun qzFunctionVisibility(program: Long, function: Int): Int =
    wasm("qz_program_function_visibility", p(program), function)

internal actual fun qzFunctionIsSuspend(program: Long, function: Int): Boolean =
    wasm("qz_program_function_is_suspend", p(program), function) and 0xFF != 0

internal actual fun qzFunctionIsExported(program: Long, function: Int): Boolean =
    wasm("qz_program_function_is_exported", p(program), function) and 0xFF != 0

internal actual fun qzImportPackage(program: Long, import: Int): String =
    text(wasm("qz_program_import_package", p(program), import))

internal actual fun qzImportIsRequired(program: Long, import: Int): Boolean =
    wasm("qz_program_import_is_required", p(program), import) and 0xFF != 0

internal actual fun qzTypePackage(program: Long, type: Int): String = text(wasm("qz_type_package", p(program), type))

internal actual fun qzAttributeCount(program: Long, function: Int): Int =
    wasm("qz_program_function_attribute_count", p(program), function)

internal actual fun qzAttributeName(program: Long, function: Int, attribute: Int): String =
    text(wasm("qz_program_function_attribute_name", p(program), function, attribute))

internal actual fun qzAttributeFilter(program: Long, function: Int, attribute: Int): Int =
    wasm("qz_program_function_attribute_filter", p(program), function, attribute)

internal actual fun qzAttributeArgCount(program: Long, function: Int, attribute: Int): Int =
    wasm("qz_program_function_attribute_arg_count", p(program), function, attribute)

internal actual fun qzAttributeArg(
    program: Long,
    function: Int,
    attribute: Int,
    index: Int,
    numbers: LongArray,
    texts: Array<String?>,
): Boolean = withBuffer(32) { arg ->
    if (wasm("qz_program_function_attribute_arg", p(program), function, attribute, index, arg) and 0xFF == 0) {
        return@withBuffer false
    }
    numbers[0] = peekI32(arg + 4).toLong()
    numbers[1] = peek64(arg + 8)
    numbers[2] = peek64(arg + 16)
    texts[0] = text(peekI32(arg))
    texts[1] = text(peekI32(arg + 24))
    true
}

internal actual fun qzTaskResult(sandbox: Long, task: Int, out: LongArray, slots: Int): Int =
    withBuffer(8 * slots) { cells ->
        val status = wasm("qz_sandbox_task_result", p(sandbox), task, cells, slots)
        for (i in 0 until slots) {
            out[i] = peek64(cells + 8 * i)
        }
        status
    }
