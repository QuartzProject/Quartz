// JNI over API level 4 of quartz.h: compiling with warning levels, diagnostics, introspection, wide task results.
#include "quartz_jni.h"

static jstring borrowed(JNIEnv* env, const char* text) {
    text = text != NULL ? text : "";
    return string(env, text, strlen(text));
}

JNIEXPORT jint QZ_JNI(qzApiLevel)(JNIEnv* env, jclass cls) {
    (void)env;
    (void)cls;
    return (jint)qz_api_level();
}

JNIEXPORT jlong QZ_JNI(qzCompileWith)(JNIEnv* env, jclass cls, jobjectArray names, jobjectArray texts, jboolean emit,
                                      jboolean lines, jstring manifest, jobjectArray rules, jintArray levels,
                                      jlongArray diagnostics) {
    (void)cls;
    jsize n = (*env)->GetArrayLength(env, names);
    jsize r = (*env)->GetArrayLength(env, rules);
    qz_source* sources = calloc((size_t)n + 1, sizeof(qz_source));
    qz_rule_level* table = calloc((size_t)r + 1, sizeof(qz_rule_level));
    jint* codes = (*env)->GetIntArrayElements(env, levels, NULL);
    for (jsize i = 0; sources != NULL && i < n; ++i) {
        jstring name = (*env)->GetObjectArrayElement(env, names, i);
        jstring text = (*env)->GetObjectArrayElement(env, texts, i);
        sources[i].name = utf8(env, name, NULL);
        sources[i].text = utf8(env, text, &sources[i].length);
        (*env)->DeleteLocalRef(env, name);
        (*env)->DeleteLocalRef(env, text);
    }
    for (jsize i = 0; table != NULL && codes != NULL && i < r; ++i) {
        jstring rule = (*env)->GetObjectArrayElement(env, rules, i);
        table[i].rule = utf8(env, rule, NULL);
        table[i].level = (qz_level)codes[i];
        (*env)->DeleteLocalRef(env, rule);
    }
    char* m = utf8(env, manifest, NULL);
    qz_compile_options options = {emit, lines, m};
    qz_diagnostics* out = NULL;
    qz_program* p = sources != NULL && table != NULL && codes != NULL
                        ? qz_compile_with(sources, (size_t)n, &options, table, (size_t)r, &out)
                        : NULL;
    for (jsize i = 0; sources != NULL && i < n; ++i) {
        free((void*)sources[i].name);
        free((void*)sources[i].text);
    }
    for (jsize i = 0; table != NULL && i < r; ++i) {
        free((void*)table[i].rule);
    }
    if (codes != NULL) {
        (*env)->ReleaseIntArrayElements(env, levels, codes, JNI_ABORT);
    }
    free(sources);
    free(table);
    free(m);
    jlong d = addr(out);
    (*env)->SetLongArrayRegion(env, diagnostics, 0, 1, &d);
    return addr(p);
}

JNIEXPORT void QZ_JNI(qzDiagnosticsFree)(JNIEnv* env, jclass cls, jlong diagnostics) {
    (void)env;
    (void)cls;
    qz_diagnostics_free(ptr(diagnostics));
}

JNIEXPORT jint QZ_JNI(qzDiagnosticsCount)(JNIEnv* env, jclass cls, jlong diagnostics) {
    (void)env;
    (void)cls;
    return (jint)qz_diagnostics_count(ptr(diagnostics));
}

JNIEXPORT jstring QZ_JNI(qzDiagnosticsText)(JNIEnv* env, jclass cls, jlong diagnostics) {
    (void)cls;
    return borrowed(env, qz_diagnostics_text(ptr(diagnostics)));
}

JNIEXPORT jint QZ_JNI(qzDiagnosticSeverity)(JNIEnv* env, jclass cls, jlong diagnostics, jint index) {
    (void)env;
    (void)cls;
    return (jint)qz_diagnostic_severity(ptr(diagnostics), (size_t)index);
}

JNIEXPORT jstring QZ_JNI(qzDiagnosticRule)(JNIEnv* env, jclass cls, jlong diagnostics, jint index) {
    (void)cls;
    return borrowed(env, qz_diagnostic_rule(ptr(diagnostics), (size_t)index));
}

JNIEXPORT jstring QZ_JNI(qzDiagnosticFile)(JNIEnv* env, jclass cls, jlong diagnostics, jint index) {
    (void)cls;
    return borrowed(env, qz_diagnostic_file(ptr(diagnostics), (size_t)index));
}

JNIEXPORT jstring QZ_JNI(qzDiagnosticMessage)(JNIEnv* env, jclass cls, jlong diagnostics, jint index) {
    (void)cls;
    return borrowed(env, qz_diagnostic_message(ptr(diagnostics), (size_t)index));
}

JNIEXPORT void QZ_JNI(qzDiagnosticRange)(JNIEnv* env, jclass cls, jlong diagnostics, jint index, jintArray out) {
    (void)cls;
    uint32_t cells[4] = {0, 0, 0, 0};
    qz_diagnostic_range(ptr(diagnostics), (size_t)index, &cells[0], &cells[1], &cells[2], &cells[3]);
    jint values[4] = {(jint)cells[0], (jint)cells[1], (jint)cells[2], (jint)cells[3]};
    (*env)->SetIntArrayRegion(env, out, 0, 4, values);
}

JNIEXPORT jint QZ_JNI(qzWarningRuleCount)(JNIEnv* env, jclass cls) {
    (void)env;
    (void)cls;
    return (jint)qz_warning_rule_count();
}

JNIEXPORT jstring QZ_JNI(qzWarningRule)(JNIEnv* env, jclass cls, jint index) {
    (void)cls;
    return borrowed(env, qz_warning_rule((size_t)index));
}

JNIEXPORT jlong QZ_JNI(qzProgramFindFunction)(JNIEnv* env, jclass cls, jlong program, jstring name) {
    (void)cls;
    char* text = utf8(env, name, NULL);
    if (text == NULL) {
        return (jlong)QZ_LOOKUP_NOT_FOUND << 32;
    }
    uint32_t index = 0;
    qz_lookup status = qz_program_find_function(ptr(program), text, &index);
    free(text);
    return ((jlong)status << 32) | index;
}

JNIEXPORT jint QZ_JNI(qzExportCount)(JNIEnv* env, jclass cls, jlong program) {
    (void)env;
    (void)cls;
    return (jint)qz_program_export_count(ptr(program));
}

JNIEXPORT jint QZ_JNI(qzExport)(JNIEnv* env, jclass cls, jlong program, jint index) {
    (void)env;
    (void)cls;
    return (jint)qz_program_export(ptr(program), (uint32_t)index);
}

JNIEXPORT jstring QZ_JNI(qzFunctionName)(JNIEnv* env, jclass cls, jlong program, jint function) {
    (void)cls;
    return borrowed(env, qz_program_function_name(ptr(program), (uint32_t)function));
}

JNIEXPORT jstring QZ_JNI(qzFunctionPackage)(JNIEnv* env, jclass cls, jlong program, jint function) {
    (void)cls;
    return borrowed(env, qz_program_function_package(ptr(program), (uint32_t)function));
}

JNIEXPORT jstring QZ_JNI(qzFunctionParamName)(JNIEnv* env, jclass cls, jlong program, jint function, jint index) {
    (void)cls;
    return borrowed(env, qz_program_function_param_name(ptr(program), (uint32_t)function, (uint32_t)index));
}

JNIEXPORT jint QZ_JNI(qzFunctionVisibility)(JNIEnv* env, jclass cls, jlong program, jint function) {
    (void)env;
    (void)cls;
    return (jint)qz_program_function_visibility(ptr(program), (uint32_t)function);
}

JNIEXPORT jboolean QZ_JNI(qzFunctionIsSuspend)(JNIEnv* env, jclass cls, jlong program, jint function) {
    (void)env;
    (void)cls;
    return qz_program_function_is_suspend(ptr(program), (uint32_t)function);
}

JNIEXPORT jboolean QZ_JNI(qzFunctionIsExported)(JNIEnv* env, jclass cls, jlong program, jint function) {
    (void)env;
    (void)cls;
    return qz_program_function_is_exported(ptr(program), (uint32_t)function);
}

JNIEXPORT jstring QZ_JNI(qzImportPackage)(JNIEnv* env, jclass cls, jlong program, jint import) {
    (void)cls;
    return borrowed(env, qz_program_import_package(ptr(program), (uint32_t)import));
}

JNIEXPORT jboolean QZ_JNI(qzImportIsRequired)(JNIEnv* env, jclass cls, jlong program, jint import) {
    (void)env;
    (void)cls;
    return qz_program_import_is_required(ptr(program), (uint32_t)import);
}

JNIEXPORT jstring QZ_JNI(qzTypePackage)(JNIEnv* env, jclass cls, jlong program, jint type) {
    (void)cls;
    return borrowed(env, qz_type_package(ptr(program), (uint32_t)type));
}

JNIEXPORT jint QZ_JNI(qzAttributeCount)(JNIEnv* env, jclass cls, jlong program, jint function) {
    (void)env;
    (void)cls;
    return (jint)qz_program_function_attribute_count(ptr(program), (uint32_t)function);
}

JNIEXPORT jstring QZ_JNI(qzAttributeName)(JNIEnv* env, jclass cls, jlong program, jint function, jint attribute) {
    (void)cls;
    return borrowed(env, qz_program_function_attribute_name(ptr(program), (uint32_t)function, (uint32_t)attribute));
}

JNIEXPORT jint QZ_JNI(qzAttributeFilter)(JNIEnv* env, jclass cls, jlong program, jint function, jint attribute) {
    (void)env;
    (void)cls;
    return (jint)qz_program_function_attribute_filter(ptr(program), (uint32_t)function, (uint32_t)attribute);
}

JNIEXPORT jint QZ_JNI(qzAttributeArgCount)(JNIEnv* env, jclass cls, jlong program, jint function, jint attribute) {
    (void)env;
    (void)cls;
    return (jint)qz_program_function_attribute_arg_count(ptr(program), (uint32_t)function, (uint32_t)attribute);
}

JNIEXPORT jboolean QZ_JNI(qzAttributeArg)(JNIEnv* env, jclass cls, jlong program, jint function, jint attribute,
                                          jint index, jlongArray numbers, jobjectArray texts) {
    (void)cls;
    qz_attribute_arg arg;
    if (!qz_program_function_attribute_arg(ptr(program), (uint32_t)function, (uint32_t)attribute, (uint32_t)index,
                                           &arg)) {
        return JNI_FALSE;
    }
    jlong values[3] = {(jlong)arg.kind, (jlong)arg.int_value, 0};
    memcpy(&values[2], &arg.float_value, sizeof(double));
    (*env)->SetLongArrayRegion(env, numbers, 0, 3, values);
    (*env)->SetObjectArrayElement(env, texts, 0, borrowed(env, arg.key));
    (*env)->SetObjectArrayElement(env, texts, 1, borrowed(env, arg.text));
    return JNI_TRUE;
}

JNIEXPORT jint QZ_JNI(qzTaskResult)(JNIEnv* env, jclass cls, jlong sandbox, jint task, jlongArray out, jint slots) {
    (void)cls;
    qz_slot* cells = calloc((size_t)slots + 1, sizeof(qz_slot));
    if (cells == NULL) {
        return QZ_ERROR;
    }
    qz_status status = qz_sandbox_task_result(ptr(sandbox), (uint32_t)task, cells, (size_t)slots);
    if (status == QZ_OK) {
        (*env)->SetLongArrayRegion(env, out, 0, slots, (const jlong*)cells);
    }
    free(cells);
    return (jint)status;
}
