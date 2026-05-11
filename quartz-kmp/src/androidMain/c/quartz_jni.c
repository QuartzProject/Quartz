// JNI over quartz.h for ART, which has no java.lang.foreign. Pointers and qz_slot cross as jlong.
#include "quartz/quartz.h"

#include <jni.h>
#include <stdlib.h>
#include <string.h>

#define QZ_JNI(name) JNICALL Java_org_quartzlang_kmp_QuartzJni_##name

static JavaVM* vm;
static jmethodID dispatchMethod;

static void* ptr(jlong p) { return (void*)(intptr_t)p; }

static jlong addr(const void* p) { return (jlong)(intptr_t)p; }

static qz_slot slot(jlong bits) {
    qz_slot s;
    s.i = bits;
    return s;
}

// UTF-16 to UTF-8, since GetStringUTFChars gives modified UTF-8; NULL for a null string or no memory.
static char* utf8(JNIEnv* env, jstring text, size_t* length) {
    if (text == NULL) {
        return NULL;
    }
    jsize n = (*env)->GetStringLength(env, text);
    const jchar* units = (*env)->GetStringChars(env, text, NULL);
    char* out = malloc((size_t)n * 3 + 1);
    size_t k = 0;
    for (jsize i = 0; out != NULL && i < n; ++i) {
        uint32_t c = units[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && units[i + 1] >= 0xDC00 && units[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (units[++i] - 0xDC00);
        } else if (c >= 0xD800 && c < 0xE000) {
            c = 0xFFFD;
        }
        if (c < 0x80) {
            out[k++] = (char)c;
        } else if (c < 0x800) {
            out[k++] = (char)(0xC0 | (c >> 6));
            out[k++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out[k++] = (char)(0xE0 | (c >> 12));
            out[k++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[k++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[k++] = (char)(0xF0 | (c >> 18));
            out[k++] = (char)(0x80 | ((c >> 12) & 0x3F));
            out[k++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[k++] = (char)(0x80 | (c & 0x3F));
        }
    }
    (*env)->ReleaseStringChars(env, text, units);
    if (out != NULL) {
        out[k] = '\0';
    }
    if (length != NULL) {
        *length = k;
    }
    return out;
}

// UTF-8 (as Quartz produces it) to a Java string; invalid bytes become U+FFFD.
static jstring string(JNIEnv* env, const char* text, size_t length) {
    if (text == NULL) {
        return NULL;
    }
    jchar* units = malloc(length * sizeof(jchar) + 1);
    if (units == NULL) {
        return NULL;
    }
    const unsigned char* s = (const unsigned char*)text;
    size_t n = 0;
    for (size_t i = 0; i < length;) {
        uint32_t c = s[i];
        size_t extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if ((c >= 0x80 && c < 0xC0) || c >= 0xF8 || (extra > 0 && i + extra >= length)) {
            units[n++] = 0xFFFD;
            ++i;
            continue;
        }
        c &= extra == 0 ? 0x7F : (0x3F >> extra);
        for (size_t k = 1; k <= extra; ++k) {
            c = (c << 6) | (s[i + k] & 0x3F);
        }
        i += extra + 1;
        if (c >= 0x10000) {
            c -= 0x10000;
            units[n++] = (jchar)(0xD800 + (c >> 10));
            units[n++] = (jchar)(0xDC00 + (c & 0x3FF));
        } else {
            units[n++] = (jchar)c;
        }
    }
    jstring out = (*env)->NewString(env, units, (jsize)n);
    free(units);
    return out;
}

static jstring take(JNIEnv* env, char* text) {
    jstring out = text != NULL ? string(env, text, strlen(text)) : NULL;
    qz_free(text);
    return out;
}

static jbyteArray bytes(JNIEnv* env, const void* data, size_t length) {
    jbyteArray out = (*env)->NewByteArray(env, (jsize)length);
    if (out != NULL && length > 0) {
        (*env)->SetByteArrayRegion(env, out, 0, (jsize)length, data);
    }
    return out;
}

static void* copyBytes(JNIEnv* env, jbyteArray array, jsize* length) {
    *length = (*env)->GetArrayLength(env, array);
    void* out = malloc((size_t)*length + 1);
    if (out != NULL) {
        (*env)->GetByteArrayRegion(env, array, 0, *length, out);
    }
    return out;
}

static jlong packType(qz_kind kind, uint32_t slots) { return ((jlong)kind << 32) | slots; }

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* javaVm, void* reserved) {
    (void)reserved;
    vm = javaVm;
    JNIEnv* env;
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }
    jclass binding = (*env)->FindClass(env, "org/quartzlang/kmp/CBinding");
    if (binding == NULL) {
        return JNI_ERR;
    }
    dispatchMethod = (*env)->GetMethodID(env, binding, "dispatch", "(JJ)Z");
    return dispatchMethod == NULL ? JNI_ERR : JNI_VERSION_1_6;
}

static bool trampoline(qz_call* call, const qz_slot* args, qz_slot* result, void* user) {
    (void)result;
    JNIEnv* env;
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        qz_call_fail(call, "host function called on a thread the JVM does not know");
        return false;
    }
    jboolean ok = (*env)->CallBooleanMethod(env, (jobject)user, dispatchMethod, addr(call), addr(args));
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        qz_call_fail(call, "host error");
        return false;
    }
    return ok;
}

JNIEXPORT jint QZ_JNI(qzAbiVersion)(JNIEnv* env, jclass cls) {
    (void)env;
    (void)cls;
    return (jint)qz_abi_version();
}

JNIEXPORT jlong QZ_JNI(qzCompile)(JNIEnv* env, jclass cls, jobjectArray names, jobjectArray texts, jboolean emit,
                                  jboolean lines, jstring manifest, jobjectArray diagnostics) {
    (void)cls;
    jsize n = (*env)->GetArrayLength(env, names);
    qz_source* sources = calloc((size_t)n + 1, sizeof(qz_source));
    for (jsize i = 0; sources != NULL && i < n; ++i) {
        jstring name = (*env)->GetObjectArrayElement(env, names, i);
        jstring text = (*env)->GetObjectArrayElement(env, texts, i);
        sources[i].name = utf8(env, name, NULL);
        sources[i].text = utf8(env, text, &sources[i].length);
        (*env)->DeleteLocalRef(env, name);
        (*env)->DeleteLocalRef(env, text);
    }
    char* m = utf8(env, manifest, NULL);
    qz_compile_options options = {emit, lines, m};
    char* out = NULL;
    qz_program* p = sources != NULL ? qz_compile(sources, (size_t)n, &options, &out) : NULL;
    for (jsize i = 0; sources != NULL && i < n; ++i) {
        free((void*)sources[i].name);
        free((void*)sources[i].text);
    }
    free(sources);
    free(m);
    (*env)->SetObjectArrayElement(env, diagnostics, 0, take(env, out));
    return addr(p);
}

JNIEXPORT jlong QZ_JNI(qzLoad)(JNIEnv* env, jclass cls, jbyteArray module, jobjectArray keys, jboolean require,
                               jobjectArray error) {
    (void)cls;
    jsize length = 0;
    void* data = copyBytes(env, module, &length);
    jsize count = (*env)->GetArrayLength(env, keys);
    uint8_t(*trusted)[32] = calloc((size_t)count + 1, 32);
    for (jsize i = 0; trusted != NULL && i < count; ++i) {
        jbyteArray key = (*env)->GetObjectArrayElement(env, keys, i);
        (*env)->GetByteArrayRegion(env, key, 0, 32, (jbyte*)trusted[i]);
        (*env)->DeleteLocalRef(env, key);
    }
    char* out = NULL;
    qz_program* p = data != NULL && trusted != NULL
                        ? qz_load(data, (size_t)length, count > 0 ? (const uint8_t(*)[32])trusted : NULL, (size_t)count,
                                  require, &out)
                        : NULL;
    free(data);
    free(trusted);
    (*env)->SetObjectArrayElement(env, error, 0, take(env, out));
    return addr(p);
}

JNIEXPORT jbyteArray QZ_JNI(qzProgramBinary)(JNIEnv* env, jclass cls, jlong program) {
    (void)cls;
    size_t length = 0;
    uint8_t* data = qz_program_binary(ptr(program), &length);
    if (data == NULL) {
        return NULL;
    }
    jbyteArray out = bytes(env, data, length);
    qz_free(data);
    return out;
}

JNIEXPORT void QZ_JNI(qzProgramFree)(JNIEnv* env, jclass cls, jlong program) {
    (void)env;
    (void)cls;
    qz_program_free(ptr(program));
}

JNIEXPORT jint QZ_JNI(qzProgramFunction)(JNIEnv* env, jclass cls, jlong program, jstring name) {
    (void)cls;
    char* n = utf8(env, name, NULL);
    jint index = n != NULL ? (jint)qz_program_function(ptr(program), n) : -1;
    free(n);
    return index;
}

JNIEXPORT jint QZ_JNI(qzFunctionArity)(JNIEnv* env, jclass cls, jlong program, jint function) {
    (void)env;
    (void)cls;
    return (jint)qz_program_function_arity(ptr(program), (uint32_t)function);
}

JNIEXPORT jlong QZ_JNI(qzFunctionParam)(JNIEnv* env, jclass cls, jlong program, jint function, jint index) {
    (void)env;
    (void)cls;
    uint32_t slots = 0;
    qz_kind kind = qz_program_function_param(ptr(program), (uint32_t)function, (uint32_t)index, &slots);
    return packType(kind, slots);
}

JNIEXPORT jlong QZ_JNI(qzFunctionResult)(JNIEnv* env, jclass cls, jlong program, jint function) {
    (void)env;
    (void)cls;
    uint32_t slots = 0;
    qz_kind kind = qz_program_function_result(ptr(program), (uint32_t)function, &slots);
    return packType(kind, slots);
}

JNIEXPORT jint QZ_JNI(qzImportCount)(JNIEnv* env, jclass cls, jlong program) {
    (void)env;
    (void)cls;
    return (jint)qz_program_import_count(ptr(program));
}

JNIEXPORT jstring QZ_JNI(qzImportName)(JNIEnv* env, jclass cls, jlong program, jint import) {
    (void)cls;
    const char* name = qz_program_import_name(ptr(program), (uint32_t)import);
    return string(env, name, strlen(name));
}

JNIEXPORT jint QZ_JNI(qzImportArity)(JNIEnv* env, jclass cls, jlong program, jint import) {
    (void)env;
    (void)cls;
    return (jint)qz_program_import_arity(ptr(program), (uint32_t)import);
}

JNIEXPORT jlong QZ_JNI(qzImportParam)(JNIEnv* env, jclass cls, jlong program, jint import, jint index) {
    (void)env;
    (void)cls;
    uint32_t slots = 0;
    qz_kind kind = qz_program_import_param(ptr(program), (uint32_t)import, (uint32_t)index, &slots);
    return packType(kind, slots);
}

JNIEXPORT jlong QZ_JNI(qzImportResult)(JNIEnv* env, jclass cls, jlong program, jint import) {
    (void)env;
    (void)cls;
    uint32_t slots = 0;
    qz_kind kind = qz_program_import_result(ptr(program), (uint32_t)import, &slots);
    return packType(kind, slots);
}

JNIEXPORT jstring QZ_JNI(qzProgramInterface)(JNIEnv* env, jclass cls, jlong program, jstring manifest,
                                             jobjectArray error) {
    (void)cls;
    char* m = utf8(env, manifest, NULL);
    char* out = NULL;
    char* text = qz_program_interface(ptr(program), m, &out);
    free(m);
    (*env)->SetObjectArrayElement(env, error, 0, take(env, out));
    return take(env, text);
}

JNIEXPORT jlong QZ_JNI(qzSandboxNew)(JNIEnv* env, jclass cls, jlong program, jlong memory, jlong fuel, jint depth,
                                     jint tier) {
    (void)env;
    (void)cls;
    qz_limits limits = {(size_t)memory, fuel, (uint32_t)depth, tier};
    return addr(qz_sandbox_new(ptr(program), &limits));
}

JNIEXPORT void QZ_JNI(qzSandboxFree)(JNIEnv* env, jclass cls, jlong sandbox) {
    (void)env;
    (void)cls;
    qz_sandbox_free(ptr(sandbox));
}

JNIEXPORT jlong QZ_JNI(qzBind)(JNIEnv* env, jclass cls, jlong sandbox, jstring name, jobject binding) {
    (void)cls;
    char* n = utf8(env, name, NULL);
    jobject ref = (*env)->NewGlobalRef(env, binding);
    bool bound = n != NULL && ref != NULL && qz_sandbox_bind_direct(ptr(sandbox), n, trampoline, ref);
    free(n);
    if (!bound) {
        if (ref != NULL) {
            (*env)->DeleteGlobalRef(env, ref);
        }
        return 0;
    }
    return addr(ref);
}

JNIEXPORT void QZ_JNI(qzUnbind)(JNIEnv* env, jclass cls, jlong token) {
    (void)cls;
    (*env)->DeleteGlobalRef(env, ptr(token));
}

JNIEXPORT jint QZ_JNI(qzStart)(JNIEnv* env, jclass cls, jlong sandbox) {
    (void)env;
    (void)cls;
    return (jint)qz_sandbox_start(ptr(sandbox));
}

JNIEXPORT jint QZ_JNI(qzCall)(JNIEnv* env, jclass cls, jlong sandbox, jint function, jlongArray args, jint count,
                              jlongArray result) {
    (void)cls;
    qz_slot argv[16];
    qz_slot* a = count <= 16 ? argv : malloc((size_t)count * sizeof(qz_slot));
    if (a == NULL) {
        return QZ_ERROR;
    }
    (*env)->GetLongArrayRegion(env, args, 0, count, (jlong*)a);
    qz_slot r = {0};
    qz_status status = qz_sandbox_call(ptr(sandbox), (uint32_t)function, a, (size_t)count, &r);
    if (a != argv) {
        free(a);
    }
    (*env)->SetLongArrayRegion(env, result, 0, 1, (const jlong*)&r);
    return (jint)status;
}

static void storeOutcome(JNIEnv* env, jlongArray out, const qz_task_outcome* o) {
    jlong v[3] = {o->kind, o->task, o->value.i};
    (*env)->SetLongArrayRegion(env, out, 0, 3, v);
}

JNIEXPORT jint QZ_JNI(qzSpawn)(JNIEnv* env, jclass cls, jlong sandbox, jint function, jlongArray args, jint count,
                               jlongArray outcome) {
    (void)cls;
    qz_slot argv[16];
    qz_slot* a = count <= 16 ? argv : malloc((size_t)count * sizeof(qz_slot));
    if (a == NULL) {
        return QZ_ERROR;
    }
    (*env)->GetLongArrayRegion(env, args, 0, count, (jlong*)a);
    qz_task_outcome o = {0};
    qz_status status = qz_sandbox_spawn(ptr(sandbox), (uint32_t)function, a, (size_t)count, &o);
    if (a != argv) {
        free(a);
    }
    storeOutcome(env, outcome, &o);
    return (jint)status;
}

JNIEXPORT jint QZ_JNI(qzResume)(JNIEnv* env, jclass cls, jlong sandbox, jint task, jlong value, jlongArray outcome) {
    (void)cls;
    qz_task_outcome o = {0};
    qz_status status = qz_sandbox_resume(ptr(sandbox), (uint32_t)task, slot(value), &o);
    storeOutcome(env, outcome, &o);
    return (jint)status;
}

JNIEXPORT void QZ_JNI(qzCancel)(JNIEnv* env, jclass cls, jlong sandbox, jint task) {
    (void)env;
    (void)cls;
    qz_sandbox_cancel(ptr(sandbox), (uint32_t)task);
}

JNIEXPORT jstring QZ_JNI(qzError)(JNIEnv* env, jclass cls, jlong sandbox) {
    (void)cls;
    const char* e = qz_sandbox_error(ptr(sandbox));
    return string(env, e, strlen(e));
}

JNIEXPORT jint QZ_JNI(qzTrapCode)(JNIEnv* env, jclass cls, jlong sandbox) {
    (void)env;
    (void)cls;
    return (jint)qz_sandbox_trap_code(ptr(sandbox));
}

JNIEXPORT jlong QZ_JNI(qzSandboxStr)(JNIEnv* env, jclass cls, jlong sandbox, jbyteArray text) {
    (void)cls;
    jsize length = 0;
    char* data = copyBytes(env, text, &length);
    jlong out = data != NULL ? qz_sandbox_str(ptr(sandbox), data, (size_t)length).i : 0;
    free(data);
    return out;
}

JNIEXPORT void QZ_JNI(qzSetFuel)(JNIEnv* env, jclass cls, jlong sandbox, jlong fuel) {
    (void)env;
    (void)cls;
    qz_sandbox_set_fuel(ptr(sandbox), fuel);
}

JNIEXPORT jlong QZ_JNI(qzFuelLeft)(JNIEnv* env, jclass cls, jlong sandbox) {
    (void)env;
    (void)cls;
    return qz_sandbox_fuel_left(ptr(sandbox));
}

JNIEXPORT jlong QZ_JNI(qzFuelPerCall)(JNIEnv* env, jclass cls, jlong sandbox) {
    (void)env;
    (void)cls;
    return qz_sandbox_fuel_per_call(ptr(sandbox));
}

JNIEXPORT jlong QZ_JNI(qzMemoryUsed)(JNIEnv* env, jclass cls, jlong sandbox) {
    (void)env;
    (void)cls;
    return (jlong)qz_sandbox_memory_used(ptr(sandbox));
}

JNIEXPORT jlong QZ_JNI(qzLiveTasks)(JNIEnv* env, jclass cls, jlong sandbox) {
    (void)env;
    (void)cls;
    return (jlong)qz_sandbox_live_tasks(ptr(sandbox));
}

JNIEXPORT jstring QZ_JNI(qzExportState)(JNIEnv* env, jclass cls, jlong sandbox) {
    (void)cls;
    return take(env, qz_sandbox_export_state(ptr(sandbox)));
}

JNIEXPORT jint QZ_JNI(qzImportState)(JNIEnv* env, jclass cls, jlong sandbox, jstring json) {
    (void)cls;
    size_t length = 0;
    char* text = utf8(env, json, &length);
    jint status = text != NULL ? (jint)qz_sandbox_import_state(ptr(sandbox), text, length) : QZ_ERROR;
    free(text);
    return status;
}

JNIEXPORT jint QZ_JNI(qzLink)(JNIEnv* env, jclass cls, jlong client, jstring pkg, jlong provider) {
    (void)cls;
    char* p = utf8(env, pkg, NULL);
    jint status = p != NULL ? (jint)qz_sandbox_link(ptr(client), p, ptr(provider)) : QZ_ERROR;
    free(p);
    return status;
}

JNIEXPORT jint QZ_JNI(qzReload)(JNIEnv* env, jclass cls, jlong sandbox, jlong program) {
    (void)env;
    (void)cls;
    return (jint)qz_sandbox_reload(ptr(sandbox), ptr(program));
}

JNIEXPORT jstring QZ_JNI(qzExportHandle)(JNIEnv* env, jclass cls, jlong sandbox, jlong handle) {
    (void)cls;
    return take(env, qz_sandbox_export_handle(ptr(sandbox), (uint64_t)handle));
}

JNIEXPORT jint QZ_JNI(qzImportHandle)(JNIEnv* env, jclass cls, jlong sandbox, jlong handle, jstring json) {
    (void)cls;
    size_t length = 0;
    char* text = utf8(env, json, &length);
    jint status = text != NULL ? (jint)qz_sandbox_import_handle(ptr(sandbox), (uint64_t)handle, text, length) : QZ_ERROR;
    free(text);
    return status;
}

JNIEXPORT void QZ_JNI(qzDropHandle)(JNIEnv* env, jclass cls, jlong sandbox, jlong handle) {
    (void)env;
    (void)cls;
    qz_sandbox_drop_handle(ptr(sandbox), (uint64_t)handle);
}

JNIEXPORT jbyteArray QZ_JNI(qzStr)(JNIEnv* env, jclass cls, jlong bits) {
    (void)cls;
    size_t length = 0;
    const char* text = qz_str(slot(bits), &length);
    return bytes(env, text, length);
}

JNIEXPORT jlong QZ_JNI(qzSlot)(JNIEnv* env, jclass cls, jlong args, jint index) {
    (void)env;
    (void)cls;
    return ((const qz_slot*)ptr(args))[index].i;
}

JNIEXPORT jlong QZ_JNI(qzCallStr)(JNIEnv* env, jclass cls, jlong call, jbyteArray text) {
    (void)cls;
    jsize length = 0;
    char* data = copyBytes(env, text, &length);
    jlong out = data != NULL ? qz_call_str(ptr(call), data, (size_t)length).i : 0;
    free(data);
    return out;
}

JNIEXPORT void QZ_JNI(qzCallSetResult)(JNIEnv* env, jclass cls, jlong call, jlong bits) {
    (void)env;
    (void)cls;
    qz_call_set_result(ptr(call), slot(bits));
}

JNIEXPORT jboolean QZ_JNI(qzCallCharge)(JNIEnv* env, jclass cls, jlong call, jlong units) {
    (void)env;
    (void)cls;
    return qz_call_charge(ptr(call), units);
}

JNIEXPORT void QZ_JNI(qzCallFail)(JNIEnv* env, jclass cls, jlong call, jstring message) {
    (void)cls;
    char* m = utf8(env, message, NULL);
    qz_call_fail(ptr(call), m != NULL ? m : "host error");
    free(m);
}

JNIEXPORT jint QZ_JNI(qzWaitType)(JNIEnv* env, jclass cls, jlong sandbox, jint task) {
    (void)env;
    (void)cls;
    return (jint)qz_sandbox_wait_type(ptr(sandbox), (uint32_t)task);
}

JNIEXPORT jlong QZ_JNI(qzSandboxList)(JNIEnv* env, jclass cls, jlong sandbox, jint type) {
    (void)env;
    (void)cls;
    return qz_sandbox_list(ptr(sandbox), (uint32_t)type).i;
}

JNIEXPORT jboolean QZ_JNI(qzListPush)(JNIEnv* env, jclass cls, jlong sandbox, jlong list, jlong element) {
    (void)env;
    (void)cls;
    qz_slot e = slot(element);
    return qz_list_push(ptr(sandbox), slot(list), &e, 1);
}

JNIEXPORT jlong QZ_JNI(qzListLength)(JNIEnv* env, jclass cls, jlong list) {
    (void)env;
    (void)cls;
    return (jlong)qz_list_length(slot(list));
}

JNIEXPORT jboolean QZ_JNI(qzListGet)(JNIEnv* env, jclass cls, jlong list, jlong index, jlongArray out) {
    (void)cls;
    qz_slot e = {0};
    bool ok = qz_list_get(slot(list), (size_t)index, &e, 1);
    (*env)->SetLongArrayRegion(env, out, 0, 1, (const jlong*)&e);
    return ok;
}
