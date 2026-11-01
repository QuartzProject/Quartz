// JNI over API level 5 of quartz.h: lossy text, DEFLATE and CRC-32, docs and boxed recursive payloads.
#include "quartz_jni.h"

static jstring doc(JNIEnv* env, const char* text) {
    text = text != NULL ? text : "";
    return string(env, text, strlen(text));
}

JNIEXPORT jlong QZ_JNI(qzSandboxStrLossy)(JNIEnv* env, jclass cls, jlong sandbox, jbyteArray text) {
    (void)cls;
    jsize length = 0;
    char* data = copyBytes(env, text, &length);
    jlong out = data != NULL ? qz_sandbox_str_lossy(ptr(sandbox), data, (size_t)length).i : 0;
    free(data);
    return out;
}

JNIEXPORT jlong QZ_JNI(qzCallStrLossy)(JNIEnv* env, jclass cls, jlong call, jbyteArray text) {
    (void)cls;
    jsize length = 0;
    char* data = copyBytes(env, text, &length);
    jlong out = data != NULL ? qz_call_str_lossy(ptr(call), data, (size_t)length).i : 0;
    free(data);
    return out;
}

JNIEXPORT jbyteArray QZ_JNI(qzDeflate)(JNIEnv* env, jclass cls, jbyteArray data, jint level, jint framing) {
    (void)cls;
    jsize length = 0;
    char* in = copyBytes(env, data, &length);
    size_t n = 0;
    uint8_t* out = in != NULL ? qz_deflate((const uint8_t*)in, (size_t)length, level, (qz_framing)framing, &n) : NULL;
    free(in);
    jbyteArray result = out != NULL ? bytes(env, out, n) : NULL;
    qz_free(out);
    return result;
}

JNIEXPORT jbyteArray QZ_JNI(qzInflate)(JNIEnv* env, jclass cls, jbyteArray data, jint framing, jint max,
                                       jobjectArray error) {
    (void)cls;
    jsize length = 0;
    char* in = copyBytes(env, data, &length);
    size_t n = 0;
    char* message = NULL;
    uint8_t* out = in != NULL ? qz_inflate((const uint8_t*)in, (size_t)length, (qz_framing)framing, (size_t)max, &n,
                                           &message)
                              : NULL;
    free(in);
    if (message != NULL) {
        (*env)->SetObjectArrayElement(env, error, 0, take(env, message));
    }
    jbyteArray result = out != NULL ? bytes(env, out, n) : NULL;
    qz_free(out);
    return result;
}

JNIEXPORT jint QZ_JNI(qzCrc32)(JNIEnv* env, jclass cls, jint crc, jbyteArray data) {
    (void)cls;
    jsize length = 0;
    char* in = copyBytes(env, data, &length);
    jint out = in != NULL ? (jint)qz_crc32((uint32_t)crc, (const uint8_t*)in, (size_t)length) : crc;
    free(in);
    return out;
}

JNIEXPORT jstring QZ_JNI(qzFunctionDoc)(JNIEnv* env, jclass cls, jlong program, jint function) {
    (void)cls;
    return doc(env, qz_program_function_doc(ptr(program), (uint32_t)function));
}

JNIEXPORT jstring QZ_JNI(qzTypeDoc)(JNIEnv* env, jclass cls, jlong program, jint type) {
    (void)cls;
    return doc(env, qz_type_doc(ptr(program), (uint32_t)type));
}

JNIEXPORT jstring QZ_JNI(qzTypeMemberDoc)(JNIEnv* env, jclass cls, jlong program, jint type, jint index) {
    (void)cls;
    return doc(env, qz_type_member_doc(ptr(program), (uint32_t)type, (uint32_t)index));
}

JNIEXPORT jboolean QZ_JNI(qzTypeVariantBoxed)(JNIEnv* env, jclass cls, jlong program, jint type, jint variant,
                                              jint index) {
    (void)env;
    (void)cls;
    return qz_type_variant_boxed(ptr(program), (uint32_t)type, (uint32_t)variant, (uint32_t)index);
}
