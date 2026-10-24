// Helpers shared by the JNI files: pointers and qz_slot cross as jlong, strings as UTF-16 Java strings.
#ifndef QUARTZ_JNI_H
#define QUARTZ_JNI_H

#include "quartz/quartz.h"

#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define QZ_JNI(name) JNICALL Java_org_quartzlang_kmp_QuartzJni_##name

static inline void* ptr(jlong p) { return (void*)(intptr_t)p; }

static inline jlong addr(const void* p) { return (jlong)(intptr_t)p; }

static inline qz_slot slot(jlong bits) {
    qz_slot s;
    s.i = bits;
    return s;
}

// UTF-16 to UTF-8, since GetStringUTFChars gives modified UTF-8; NULL for a null string or no memory.
static inline char* utf8(JNIEnv* env, jstring text, size_t* length) {
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
static inline jstring string(JNIEnv* env, const char* text, size_t length) {
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

static inline jstring take(JNIEnv* env, char* text) {
    jstring out = text != NULL ? string(env, text, strlen(text)) : NULL;
    qz_free(text);
    return out;
}

static inline jbyteArray bytes(JNIEnv* env, const void* data, size_t length) {
    jbyteArray out = (*env)->NewByteArray(env, (jsize)length);
    if (out != NULL && length > 0) {
        (*env)->SetByteArrayRegion(env, out, 0, (jsize)length, data);
    }
    return out;
}

static inline void* copyBytes(JNIEnv* env, jbyteArray array, jsize* length) {
    *length = (*env)->GetArrayLength(env, array);
    void* out = malloc((size_t)*length + 1);
    if (out != NULL) {
        (*env)->GetByteArrayRegion(env, array, 0, *length, out);
    }
    return out;
}

#endif
