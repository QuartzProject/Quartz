@file:OptIn(ExperimentalForeignApi::class)

package org.quartzlang.kmp

import cnames.structs.qz_diagnostics
import cnames.structs.qz_program
import cnames.structs.qz_sandbox
import kotlinx.cinterop.ByteVar
import kotlinx.cinterop.CPointer
import kotlinx.cinterop.CPointerVar
import kotlinx.cinterop.ExperimentalForeignApi
import kotlinx.cinterop.UIntVar
import kotlinx.cinterop.addressOf
import kotlinx.cinterop.alloc
import kotlinx.cinterop.allocArray
import kotlinx.cinterop.cstr
import kotlinx.cinterop.get
import kotlinx.cinterop.memScoped
import kotlinx.cinterop.ptr
import kotlinx.cinterop.reinterpret
import kotlinx.cinterop.set
import kotlinx.cinterop.toCPointer
import kotlinx.cinterop.toKString
import kotlinx.cinterop.toLong
import kotlinx.cinterop.usePinned
import kotlinx.cinterop.value
import org.quartzlang.kmp.cinterop.qz_api_level
import org.quartzlang.kmp.cinterop.qz_attribute_arg
import org.quartzlang.kmp.cinterop.qz_compile_options
import org.quartzlang.kmp.cinterop.qz_compile_with
import org.quartzlang.kmp.cinterop.qz_diagnostic_file
import org.quartzlang.kmp.cinterop.qz_diagnostic_message
import org.quartzlang.kmp.cinterop.qz_diagnostic_range
import org.quartzlang.kmp.cinterop.qz_diagnostic_rule
import org.quartzlang.kmp.cinterop.qz_diagnostic_severity
import org.quartzlang.kmp.cinterop.qz_diagnostics_count
import org.quartzlang.kmp.cinterop.qz_diagnostics_free
import org.quartzlang.kmp.cinterop.qz_diagnostics_text
import org.quartzlang.kmp.cinterop.qz_program_export
import org.quartzlang.kmp.cinterop.qz_program_export_count
import org.quartzlang.kmp.cinterop.qz_program_find_function
import org.quartzlang.kmp.cinterop.qz_program_function_attribute_arg
import org.quartzlang.kmp.cinterop.qz_program_function_attribute_arg_count
import org.quartzlang.kmp.cinterop.qz_program_function_attribute_count
import org.quartzlang.kmp.cinterop.qz_program_function_attribute_filter
import org.quartzlang.kmp.cinterop.qz_program_function_attribute_name
import org.quartzlang.kmp.cinterop.qz_program_function_is_exported
import org.quartzlang.kmp.cinterop.qz_program_function_is_suspend
import org.quartzlang.kmp.cinterop.qz_program_function_name
import org.quartzlang.kmp.cinterop.qz_program_function_package
import org.quartzlang.kmp.cinterop.qz_program_function_param_name
import org.quartzlang.kmp.cinterop.qz_program_function_visibility
import org.quartzlang.kmp.cinterop.qz_program_import_is_required
import org.quartzlang.kmp.cinterop.qz_program_import_package
import org.quartzlang.kmp.cinterop.qz_rule_level
import org.quartzlang.kmp.cinterop.qz_sandbox_task_result
import org.quartzlang.kmp.cinterop.qz_source
import org.quartzlang.kmp.cinterop.qz_type_package
import org.quartzlang.kmp.cinterop.qz_warning_rule
import org.quartzlang.kmp.cinterop.qz_warning_rule_count

// The functions of API level 4: compiling with warning levels, diagnostics, introspection and wide task results.

private fun prog(p: Long): CPointer<qz_program> = p.toCPointer()!!

private fun diag(d: Long): CPointer<qz_diagnostics> = d.toCPointer()!!

private fun text(s: CPointer<ByteVar>?): String = s?.toKString() ?: ""

internal actual fun qzApiLevel(): Int = qz_api_level().toInt()

internal actual fun qzCompileWith(
    names: Array<String>,
    texts: Array<String>,
    emitBinary: Boolean,
    keepLines: Boolean,
    manifest: String?,
    rules: Array<String>,
    levels: IntArray,
    diagnostics: LongArray,
): Long = memScoped {
    val sources = allocArray<qz_source>(maxOf(1, names.size))
    for (i in names.indices) {
        val utf8 = texts[i].encodeToByteArray()
        sources[i].name = names[i].cstr.ptr
        val bytes = allocArray<ByteVar>(maxOf(1, utf8.size))
        utf8.forEachIndexed { k, v -> bytes[k] = v }
        sources[i].text = bytes
        sources[i].length = utf8.size.toULong()
    }
    val options = alloc<qz_compile_options>()
    options.emit_binary = emitBinary
    options.keep_lines = keepLines
    options.manifest = manifest?.cstr?.ptr
    val table = allocArray<qz_rule_level>(maxOf(1, rules.size))
    for (i in rules.indices) {
        table[i].rule = rules[i].cstr.ptr
        table[i].level = levels[i].toUInt()
    }
    val out = alloc<CPointerVar<qz_diagnostics>>()
    val p = qz_compile_with(sources, names.size.toULong(), options.ptr, table, rules.size.toULong(), out.ptr)
    diagnostics[0] = out.value.toLong()
    p.toLong()
}

internal actual fun qzDiagnosticsFree(diagnostics: Long) = qz_diagnostics_free(diag(diagnostics))

internal actual fun qzDiagnosticsCount(diagnostics: Long): Int = qz_diagnostics_count(diag(diagnostics)).toInt()

internal actual fun qzDiagnosticsText(diagnostics: Long): String = text(qz_diagnostics_text(diag(diagnostics)))

internal actual fun qzDiagnosticSeverity(diagnostics: Long, index: Int): Int =
    qz_diagnostic_severity(diag(diagnostics), index.toULong()).toInt()

internal actual fun qzDiagnosticRule(diagnostics: Long, index: Int): String =
    text(qz_diagnostic_rule(diag(diagnostics), index.toULong()))

internal actual fun qzDiagnosticFile(diagnostics: Long, index: Int): String =
    text(qz_diagnostic_file(diag(diagnostics), index.toULong()))

internal actual fun qzDiagnosticMessage(diagnostics: Long, index: Int): String =
    text(qz_diagnostic_message(diag(diagnostics), index.toULong()))

internal actual fun qzDiagnosticRange(diagnostics: Long, index: Int, out: IntArray) = memScoped {
    val cells = List(4) { alloc<UIntVar>() }
    qz_diagnostic_range(diag(diagnostics), index.toULong(), cells[0].ptr, cells[1].ptr, cells[2].ptr, cells[3].ptr)
    cells.forEachIndexed { i, cell -> out[i] = cell.value.toInt() }
}

internal actual fun qzWarningRuleCount(): Int = qz_warning_rule_count().toInt()

internal actual fun qzWarningRule(index: Int): String = text(qz_warning_rule(index.toULong()))

internal actual fun qzProgramFindFunction(program: Long, name: String): Long = memScoped {
    val index = alloc<UIntVar>()
    val status = qz_program_find_function(prog(program), name, index.ptr)
    (status.toLong() shl 32) or index.value.toLong()
}

internal actual fun qzExportCount(program: Long): Int = qz_program_export_count(prog(program)).toInt()

internal actual fun qzExport(program: Long, index: Int): Int = qz_program_export(prog(program), index.toUInt()).toInt()

internal actual fun qzFunctionName(program: Long, function: Int): String =
    text(qz_program_function_name(prog(program), function.toUInt()))

internal actual fun qzFunctionPackage(program: Long, function: Int): String =
    text(qz_program_function_package(prog(program), function.toUInt()))

internal actual fun qzFunctionParamName(program: Long, function: Int, index: Int): String =
    text(qz_program_function_param_name(prog(program), function.toUInt(), index.toUInt()))

internal actual fun qzFunctionVisibility(program: Long, function: Int): Int =
    qz_program_function_visibility(prog(program), function.toUInt()).toInt()

internal actual fun qzFunctionIsSuspend(program: Long, function: Int): Boolean =
    qz_program_function_is_suspend(prog(program), function.toUInt())

internal actual fun qzFunctionIsExported(program: Long, function: Int): Boolean =
    qz_program_function_is_exported(prog(program), function.toUInt())

internal actual fun qzImportPackage(program: Long, import: Int): String =
    text(qz_program_import_package(prog(program), import.toUInt()))

internal actual fun qzImportIsRequired(program: Long, import: Int): Boolean =
    qz_program_import_is_required(prog(program), import.toUInt())

internal actual fun qzTypePackage(program: Long, type: Int): String =
    text(qz_type_package(prog(program), type.toUInt()))

internal actual fun qzAttributeCount(program: Long, function: Int): Int =
    qz_program_function_attribute_count(prog(program), function.toUInt()).toInt()

internal actual fun qzAttributeName(program: Long, function: Int, attribute: Int): String =
    text(qz_program_function_attribute_name(prog(program), function.toUInt(), attribute.toUInt()))

internal actual fun qzAttributeFilter(program: Long, function: Int, attribute: Int): Int =
    qz_program_function_attribute_filter(prog(program), function.toUInt(), attribute.toUInt()).toInt()

internal actual fun qzAttributeArgCount(program: Long, function: Int, attribute: Int): Int =
    qz_program_function_attribute_arg_count(prog(program), function.toUInt(), attribute.toUInt()).toInt()

internal actual fun qzAttributeArg(
    program: Long,
    function: Int,
    attribute: Int,
    index: Int,
    numbers: LongArray,
    texts: Array<String?>,
): Boolean = memScoped {
    val arg = alloc<qz_attribute_arg>()
    if (!qz_program_function_attribute_arg(prog(program), function.toUInt(), attribute.toUInt(), index.toUInt(),
            arg.ptr)) {
        return@memScoped false
    }
    numbers[0] = arg.kind.toLong()
    numbers[1] = arg.int_value
    numbers[2] = arg.float_value.toRawBits()
    texts[0] = text(arg.key)
    texts[1] = text(arg.text)
    true
}

internal actual fun qzTaskResult(sandbox: Long, task: Int, out: LongArray, slots: Int): Int = out.usePinned {
    qz_sandbox_task_result(sandbox.toCPointer<qz_sandbox>(), task.toUInt(), it.addressOf(0).reinterpret(),
        slots.toULong()).toInt()
}
