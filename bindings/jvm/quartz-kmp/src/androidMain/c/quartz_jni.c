// JNI over quartz.h for ART, which has no java.lang.foreign. Pointers and qz_slot cross as jlong.
#include "quartz_jni.h"

static JavaVM* vm;
static jmethodID dispatchMethod;

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
    dispatchMethod = (*env)->GetMethodID(env, binding, "dispatch", "(JJJ)Z");
    return dispatchMethod == NULL ? JNI_ERR : JNI_VERSION_1_6;
}

static bool trampoline(qz_call* call, const qz_slot* args, qz_slot* result, void* user) {
    JNIEnv* env;
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        qz_call_fail(call, "host function called on a thread the JVM does not know");
        return false;
    }
    jboolean ok = (*env)->CallBooleanMethod(env, (jobject)user, dispatchMethod, addr(call), addr(args), addr(result));
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

JNIEXPORT jint QZ_JNI(qzFunctionArity)(JNIEnv* env, jclass cls, jlong program, jint function) {
    (void)env;
    (void)cls;
    return (jint)qz_program_function_arity(ptr(program), (uint32_t)function);
}

JNIEXPORT jint QZ_JNI(qzFunctionParamType)(JNIEnv* env, jclass cls, jlong program, jint function, jint index) {
    (void)env;
    (void)cls;
    return (jint)qz_program_function_param_type(ptr(program), (uint32_t)function, (uint32_t)index);
}

JNIEXPORT jint QZ_JNI(qzFunctionResultType)(JNIEnv* env, jclass cls, jlong program, jint function) {
    (void)env;
    (void)cls;
    return (jint)qz_program_function_result_type(ptr(program), (uint32_t)function);
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

JNIEXPORT jint QZ_JNI(qzImportParamType)(JNIEnv* env, jclass cls, jlong program, jint import, jint index) {
    (void)env;
    (void)cls;
    return (jint)qz_program_import_param_type(ptr(program), (uint32_t)import, (uint32_t)index);
}

JNIEXPORT jint QZ_JNI(qzImportResultType)(JNIEnv* env, jclass cls, jlong program, jint import) {
    (void)env;
    (void)cls;
    return (jint)qz_program_import_result_type(ptr(program), (uint32_t)import);
}

JNIEXPORT jboolean QZ_JNI(qzImportIsNative)(JNIEnv* env, jclass cls, jlong program, jint import) {
    (void)env;
    (void)cls;
    return qz_program_import_is_native(ptr(program), (uint32_t)import);
}

JNIEXPORT jlong QZ_JNI(qzTypeKind)(JNIEnv* env, jclass cls, jlong program, jint type) {
    (void)env;
    (void)cls;
    uint32_t slots = 0;
    qz_kind kind = qz_type_kind(ptr(program), (uint32_t)type, &slots);
    return packType(kind, slots);
}

JNIEXPORT jint QZ_JNI(qzTypeElem)(JNIEnv* env, jclass cls, jlong program, jint type) {
    (void)env;
    (void)cls;
    return (jint)qz_type_elem(ptr(program), (uint32_t)type);
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

JNIEXPORT jint QZ_JNI(qzCallWide)(JNIEnv* env, jclass cls, jlong sandbox, jint function, jlongArray args, jint count,
                                  jlongArray result, jint slots) {
    (void)cls;
    qz_slot* a = malloc(((size_t)count + 1) * sizeof(qz_slot));
    qz_slot* r = malloc(((size_t)slots + 1) * sizeof(qz_slot));
    qz_status status = QZ_ERROR;
    if (a != NULL && r != NULL) {
        (*env)->GetLongArrayRegion(env, args, 0, count, (jlong*)a);
        status = qz_sandbox_call_wide(ptr(sandbox), (uint32_t)function, a, (size_t)count, r, (size_t)slots);
        if (status == QZ_OK) {
            (*env)->SetLongArrayRegion(env, result, 0, slots, (const jlong*)r);
        }
    }
    free(a);
    free(r);
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

JNIEXPORT void QZ_JNI(qzSetSlot)(JNIEnv* env, jclass cls, jlong address, jint index, jlong bits) {
    (void)env;
    (void)cls;
    ((qz_slot*)ptr(address))[index].i = bits;
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

// A copy of a long[] as slots, in `stack` when it fits; free it with release().
static qz_slot* slotsOf(JNIEnv* env, jlongArray array, qz_slot* stack, size_t room) {
    jsize n = (*env)->GetArrayLength(env, array);
    qz_slot* out = (size_t)n <= room ? stack : malloc(((size_t)n + 1) * sizeof(qz_slot));
    if (out != NULL) {
        (*env)->GetLongArrayRegion(env, array, 0, n, (jlong*)out);
    }
    return out;
}

static void release(qz_slot* slots, qz_slot* stack) {
    if (slots != stack) {
        free(slots);
    }
}

JNIEXPORT jboolean QZ_JNI(qzListPush)(JNIEnv* env, jclass cls, jlong sandbox, jlong list, jlongArray element,
                                      jint slots) {
    (void)cls;
    qz_slot stack[8];
    qz_slot* e = slotsOf(env, element, stack, 8);
    bool ok = e != NULL && qz_list_push(ptr(sandbox), slot(list), e, (size_t)slots);
    release(e, stack);
    return ok;
}

JNIEXPORT jlong QZ_JNI(qzListLength)(JNIEnv* env, jclass cls, jlong list) {
    (void)env;
    (void)cls;
    return (jlong)qz_list_length(slot(list));
}

JNIEXPORT jboolean QZ_JNI(qzListGet)(JNIEnv* env, jclass cls, jlong list, jlong index, jlongArray out, jint slots) {
    (void)cls;
    qz_slot e[8];
    qz_slot* cell = slots <= 8 ? e : malloc((size_t)slots * sizeof(qz_slot));
    if (cell == NULL) {
        return false;
    }
    bool ok = qz_list_get(slot(list), (size_t)index, cell, (size_t)slots);
    if (ok) {
        (*env)->SetLongArrayRegion(env, out, 0, slots, (const jlong*)cell);
    }
    if (cell != e) {
        free(cell);
    }
    return ok;
}

JNIEXPORT jint QZ_JNI(qzTypeFieldCount)(JNIEnv* env, jclass cls, jlong program, jint type) {
    (void)env;
    (void)cls;
    return (jint)qz_type_field_count(ptr(program), (uint32_t)type);
}

JNIEXPORT jlong QZ_JNI(qzTypeField)(JNIEnv* env, jclass cls, jlong program, jint type, jint index) {
    (void)env;
    (void)cls;
    uint32_t offset = 0;
    uint32_t id = qz_type_field(ptr(program), (uint32_t)type, (uint32_t)index, &offset);
    return ((jlong)id << 32) | offset;
}

JNIEXPORT jstring QZ_JNI(qzTypeFieldName)(JNIEnv* env, jclass cls, jlong program, jint type, jint index) {
    (void)cls;
    const char* name = qz_type_field_name(ptr(program), (uint32_t)type, (uint32_t)index);
    return string(env, name, strlen(name));
}

JNIEXPORT jint QZ_JNI(qzTypeVariantCount)(JNIEnv* env, jclass cls, jlong program, jint type) {
    (void)env;
    (void)cls;
    return (jint)qz_type_variant_count(ptr(program), (uint32_t)type);
}

JNIEXPORT jstring QZ_JNI(qzTypeVariantName)(JNIEnv* env, jclass cls, jlong program, jint type, jint variant) {
    (void)cls;
    const char* name = qz_type_variant_name(ptr(program), (uint32_t)type, (uint32_t)variant);
    return string(env, name, strlen(name));
}

JNIEXPORT jint QZ_JNI(qzTypeVariantPayloadCount)(JNIEnv* env, jclass cls, jlong program, jint type, jint variant) {
    (void)env;
    (void)cls;
    return (jint)qz_type_variant_payload_count(ptr(program), (uint32_t)type, (uint32_t)variant);
}

JNIEXPORT jlong QZ_JNI(qzTypeVariantPayload)(JNIEnv* env, jclass cls, jlong program, jint type, jint variant,
                                             jint index) {
    (void)env;
    (void)cls;
    uint32_t offset = 0;
    uint32_t id = qz_type_variant_payload(ptr(program), (uint32_t)type, (uint32_t)variant, (uint32_t)index, &offset);
    return ((jlong)id << 32) | offset;
}

JNIEXPORT jboolean QZ_JNI(qzTypeNullableIsPointer)(JNIEnv* env, jclass cls, jlong program, jint type) {
    (void)env;
    (void)cls;
    return qz_type_nullable_is_pointer(ptr(program), (uint32_t)type);
}

JNIEXPORT jint QZ_JNI(qzTypeKey)(JNIEnv* env, jclass cls, jlong program, jint type) {
    (void)env;
    (void)cls;
    return (jint)qz_type_key(ptr(program), (uint32_t)type);
}

JNIEXPORT jint QZ_JNI(qzResumeWide)(JNIEnv* env, jclass cls, jlong sandbox, jint task, jlongArray value, jint slots,
                                    jlongArray outcome) {
    (void)cls;
    qz_slot stack[8];
    qz_slot* v = slotsOf(env, value, stack, 8);
    if (v == NULL) {
        return QZ_ERROR;
    }
    qz_task_outcome o = {0};
    qz_status status = qz_sandbox_resume_wide(ptr(sandbox), (uint32_t)task, v, (size_t)slots, &o);
    release(v, stack);
    storeOutcome(env, outcome, &o);
    return (jint)status;
}

JNIEXPORT jlong QZ_JNI(qzSandboxMap)(JNIEnv* env, jclass cls, jlong sandbox, jint type) {
    (void)env;
    (void)cls;
    return qz_sandbox_map(ptr(sandbox), (uint32_t)type).i;
}

JNIEXPORT jboolean QZ_JNI(qzMapSet)(JNIEnv* env, jclass cls, jlong sandbox, jlong map, jlongArray key,
                                    jlongArray value) {
    (void)cls;
    qz_slot keyStack[8];
    qz_slot valueStack[8];
    qz_slot* k = slotsOf(env, key, keyStack, 8);
    qz_slot* v = slotsOf(env, value, valueStack, 8);
    bool ok = k != NULL && v != NULL && qz_map_set(ptr(sandbox), slot(map), k, v);
    release(k, keyStack);
    release(v, valueStack);
    return ok;
}

JNIEXPORT jlong QZ_JNI(qzMapLength)(JNIEnv* env, jclass cls, jlong map) {
    (void)env;
    (void)cls;
    return (jlong)qz_map_length(slot(map));
}

JNIEXPORT jboolean QZ_JNI(qzMapEntry)(JNIEnv* env, jclass cls, jlong sandbox, jlong map, jlong index, jlongArray key,
                                      jlongArray value) {
    (void)cls;
    qz_slot keyStack[8];
    qz_slot valueStack[8];
    qz_slot* k = slotsOf(env, key, keyStack, 8);
    qz_slot* v = slotsOf(env, value, valueStack, 8);
    bool ok = k != NULL && v != NULL && qz_map_entry(ptr(sandbox), slot(map), (size_t)index, k, v);
    if (ok) {
        (*env)->SetLongArrayRegion(env, key, 0, (*env)->GetArrayLength(env, key), (const jlong*)k);
        (*env)->SetLongArrayRegion(env, value, 0, (*env)->GetArrayLength(env, value), (const jlong*)v);
    }
    release(k, keyStack);
    release(v, valueStack);
    return ok;
}

JNIEXPORT jlong QZ_JNI(qzSandboxBytes)(JNIEnv* env, jclass cls, jlong sandbox, jbyteArray bytes) {
    (void)cls;
    jsize length = 0;
    char* data = copyBytes(env, bytes, &length);
    jlong out = data != NULL ? qz_sandbox_bytes(ptr(sandbox), (const uint8_t*)data, (size_t)length).i : 0;
    free(data);
    return out;
}

JNIEXPORT jlong QZ_JNI(qzCallBytes)(JNIEnv* env, jclass cls, jlong call, jbyteArray bytes) {
    (void)cls;
    jsize length = 0;
    char* data = copyBytes(env, bytes, &length);
    jlong out = data != NULL ? qz_call_bytes(ptr(call), (const uint8_t*)data, (size_t)length).i : 0;
    free(data);
    return out;
}

JNIEXPORT jlong QZ_JNI(qzProgramFindImport)(JNIEnv* env, jclass cls, jlong program, jstring name) {
    (void)cls;
    size_t length = 0;
    char* text = utf8(env, name, &length);
    if (text == NULL) {
        return (jlong)QZ_LOOKUP_NOT_FOUND << 32;
    }
    uint32_t index = 0;
    qz_lookup status = qz_program_find_import(ptr(program), text, &index);
    free(text);
    return ((jlong)status << 32) | index;
}

JNIEXPORT jstring QZ_JNI(qzTypeName)(JNIEnv* env, jclass cls, jlong program, jint type) {
    (void)cls;
    const char* name = qz_type_name(ptr(program), (uint32_t)type);
    name = name != NULL ? name : "";
    return string(env, name, strlen(name));
}
