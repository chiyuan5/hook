/*
 * This file is part of AliuHook, a library providing XposedAPI bindings to LSPlant
 */

#include <jni.h>
#include <android/api-level.h>
#include <dlfcn.h>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

#include "elf_img.h"
#include "log.h"
#include "profile_saver.h"
#include "hidden_api.h"
#include <dobby.h>
#include "aliuhook.h"
#include "invoke_constructor.h"

int AliuHook::android_version = -1;
pine::ElfImg AliuHook::elf_img;

static size_t page_size_;
static bool g_lsplant_inited = false;
static void *g_lsplant_handle = nullptr;

#define ALIGN_DOWN(addr, page_size) ((addr) & -(page_size))
#define ALIGN_UP(addr, page_size) (((addr) + ((page_size) - 1)) & ~((page_size) - 1))

void AliuHook::init(int version) {
    elf_img.Init("libart.so", version);
    android_version = version;
}

static bool Unprotect(void *addr) {
    auto addr_uint = reinterpret_cast<uintptr_t>(addr);
    auto page_aligned_ptr = reinterpret_cast<void *>(ALIGN_DOWN(addr_uint, page_size_));
    size_t size = page_size_;

    if (ALIGN_UP(addr_uint + page_size_, page_size_) != ALIGN_UP(addr_uint, page_size_)) {
        size += page_size_;
    }

    int result = mprotect(page_aligned_ptr, size, PROT_READ | PROT_WRITE | PROT_EXEC);
    if (result == -1) {
        LOGE("mprotect failed for %p: %s (%d)", addr, strerror(errno), errno);
        return false;
    }
    return true;
}

static void *InlineHooker(void *address, void *replacement) {
    if (!Unprotect(address)) {
        return nullptr;
    }

    void *origin_call = nullptr;
    if (DobbyHook(address, replacement, &origin_call) == RS_SUCCESS) {
        return origin_call;
    }
    return nullptr;
}

static bool InlineUnhooker(void *func) {
    return DobbyDestroy(func) == RT_SUCCESS;
}

// ===== LSPlant exported C API typedefs =====

using LspInlineHooker = void *(*)(void *target, void *hooker);
using LspInlineUnhooker = bool (*)(void *func);
using LspArtSymbolResolver = void *(*)(const char *symbol_name);
using LspArtSymbolPrefixResolver = void *(*)(const char *symbol_prefix);

using FnLspInit = bool (*)(JNIEnv *env,
                           LspInlineHooker inline_hooker,
                           LspInlineUnhooker inline_unhooker,
                           LspArtSymbolResolver art_symbol_resolver,
                           LspArtSymbolPrefixResolver art_symbol_prefix_resolver);

using FnLspHook = jobject (*)(JNIEnv *env,
                              jobject target_method,
                              jobject hooker_object,
                              jobject callback_method);

using FnLspUnhook = bool (*)(JNIEnv *env, jobject target_method);
using FnLspIsHooked = bool (*)(JNIEnv *env, jobject method);
using FnLspDeoptimize = bool (*)(JNIEnv *env, jobject method);
using FnLspMakeClassInheritable = bool (*)(JNIEnv *env, jclass clazz);

static FnLspInit g_fn_init = nullptr;
static FnLspHook g_fn_hook = nullptr;
static FnLspUnhook g_fn_unhook = nullptr;
static FnLspIsHooked g_fn_is_hooked = nullptr;
static FnLspDeoptimize g_fn_deoptimize = nullptr;
static FnLspMakeClassInheritable g_fn_make_class_inheritable = nullptr;

// ===== resolvers passed into lsplant =====

static void *ResolveArtSymbol(const char *symbol_name) {
    if (!symbol_name) return nullptr;
    return AliuHook::elf_img.GetSymbolAddress(symbol_name, false, false);
}

static void *ResolveArtSymbolPrefix(const char *symbol_prefix) {
    if (!symbol_prefix) return nullptr;
    return AliuHook::elf_img.GetSymbolAddress(symbol_prefix, false, true);
}

static bool ResolveLsplantSymbols(void *handle) {
    dlerror();

    g_fn_init = reinterpret_cast<FnLspInit>(dlsym(handle, "lsp_native_init"));
    g_fn_hook = reinterpret_cast<FnLspHook>(dlsym(handle, "lsp_native_hook"));
    g_fn_unhook = reinterpret_cast<FnLspUnhook>(dlsym(handle, "lsp_native_unhook"));
    g_fn_is_hooked = reinterpret_cast<FnLspIsHooked>(dlsym(handle, "lsp_native_is_hooked"));
    g_fn_deoptimize = reinterpret_cast<FnLspDeoptimize>(dlsym(handle, "lsp_native_deoptimize"));
    g_fn_make_class_inheritable =
            reinterpret_cast<FnLspMakeClassInheritable>(dlsym(handle, "lsp_native_make_class_inheritable"));

    const char *err = dlerror();
    if (err != nullptr) {
        LOGE("dlsym failed: %s", err);
        return false;
    }

    return g_fn_init && g_fn_hook && g_fn_unhook &&
           g_fn_is_hooked && g_fn_deoptimize && g_fn_make_class_inheritable;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_de_robv_android_xposed_XposedBridge_nativeInitLsplant(JNIEnv *env, jclass, jstring lsplantPath_) {
    if (g_lsplant_inited) {
        return JNI_TRUE;
    }

    if (lsplantPath_ == nullptr) {
        LOGE("lsplant path is null");
        return JNI_FALSE;
    }

    const char *lsplant_path = env->GetStringUTFChars(lsplantPath_, nullptr);
    if (lsplant_path == nullptr) {
        return JNI_FALSE;
    }

    int api_level = android_get_device_api_level();
    if (api_level <= 0) {
        LOGE("Invalid SDK int %i", api_level);
        env->ReleaseStringUTFChars(lsplantPath_, lsplant_path);
        return JNI_FALSE;
    }

    AliuHook::init(api_level);

    // RTLD_NOW: 立刻解析
    // RTLD_GLOBAL: 让它导出的全局符号对后续库可见
    g_lsplant_handle = dlopen(lsplant_path, RTLD_NOW | RTLD_GLOBAL);
    if (!g_lsplant_handle) {
        LOGE("dlopen lsplant failed: %s", dlerror());
        env->ReleaseStringUTFChars(lsplantPath_, lsplant_path);
        return JNI_FALSE;
    }

    env->ReleaseStringUTFChars(lsplantPath_, lsplant_path);

    if (!ResolveLsplantSymbols(g_lsplant_handle)) {
        LOGE("resolve lsplant exported symbols failed");
        dlclose(g_lsplant_handle);
        g_lsplant_handle = nullptr;
        return JNI_FALSE;
    }

    bool res = g_fn_init(
            env,
            InlineHooker,
            InlineUnhooker,
            ResolveArtSymbol,
            ResolveArtSymbolPrefix
    );
    if (!res) {
        LOGE("lsp_native_init failed");
        dlclose(g_lsplant_handle);
        g_lsplant_handle = nullptr;
        return JNI_FALSE;
    }

    LOGI("lsplant custom init finished");

    res = LoadInvokeConstructorCache(env, AliuHook::android_version);
    if (!res) {
        LOGE("invoke_constructor init failed");
        dlclose(g_lsplant_handle);
        g_lsplant_handle = nullptr;
        return JNI_FALSE;
    }

    g_lsplant_inited = true;
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_de_robv_android_xposed_XposedBridge_isHooked0(JNIEnv *env, jclass, jobject method) {
    if (!g_lsplant_inited || !g_fn_is_hooked) return JNI_FALSE;
    return g_fn_is_hooked(env, method);
}

extern "C" JNIEXPORT jobject JNICALL
Java_de_robv_android_xposed_XposedBridge_hook0(JNIEnv *env, jclass, jobject context, jobject original, jobject callback) {
    if (!g_lsplant_inited || !g_fn_hook) return nullptr;
    return g_fn_hook(env, original, context, callback);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_de_robv_android_xposed_XposedBridge_unhook0(JNIEnv *env, jclass, jobject target) {
    if (!g_lsplant_inited || !g_fn_unhook) return JNI_FALSE;
    return g_fn_unhook(env, target);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_de_robv_android_xposed_XposedBridge_deoptimize0(JNIEnv *env, jclass, jobject method) {
    if (!g_lsplant_inited || !g_fn_deoptimize) return JNI_FALSE;
    return g_fn_deoptimize(env, method);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_de_robv_android_xposed_XposedBridge_makeClassInheritable0(JNIEnv *env, jclass, jclass clazz) {
    if (!g_lsplant_inited || !g_fn_make_class_inheritable) return JNI_FALSE;
    return g_fn_make_class_inheritable(env, clazz);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_de_robv_android_xposed_XposedBridge_disableProfileSaver(JNIEnv *, jclass) {
    return disable_profile_saver();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_de_robv_android_xposed_XposedBridge_disableHiddenApiRestrictions(JNIEnv *env, jclass) {
    return disable_hidden_api(env);
}

extern "C" JNIEXPORT jobject JNICALL
Java_de_robv_android_xposed_XposedBridge_allocateInstance0(JNIEnv *env, jclass, jclass clazz) {
    return env->AllocObject(clazz);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_de_robv_android_xposed_XposedBridge_invokeConstructor0(
        JNIEnv *env,
        jclass,
        jobject instance,
        jobject constructor,
        jobjectArray args) {
    jmethodID constructorMethodId = env->FromReflectedMethod(constructor);
    if (!constructorMethodId) return JNI_FALSE;

    if (!args) {
        env->CallVoidMethod(instance, constructorMethodId);
        return JNI_TRUE;
    } else {
        return InvokeConstructorWithArgs(env, instance, constructor, args);
    }
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *) {
    JNIEnv *env = nullptr;
    if (vm->GetEnv((void **) &env, JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }

    page_size_ = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM *vm, void *) {
    JNIEnv *env = nullptr;
    if (vm->GetEnv((void **) &env, JNI_VERSION_1_6) == JNI_OK) {
        UnloadInvokeConstructorCache(env);
    }

    if (g_lsplant_handle) {
        dlclose(g_lsplant_handle);
        g_lsplant_handle = nullptr;
    }

    g_lsplant_inited = false;
    g_fn_init = nullptr;
    g_fn_hook = nullptr;
    g_fn_unhook = nullptr;
    g_fn_is_hooked = nullptr;
    g_fn_deoptimize = nullptr;
    g_fn_make_class_inheritable = nullptr;
}
