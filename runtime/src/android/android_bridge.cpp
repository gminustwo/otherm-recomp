// The JNI calls between the game and its Android activity (android/java/io/github/gminustwo/otherm/OtherMActivity.java).
#include "android_bridge.h"

#include <SDL3/SDL_system.h>
#include <jni.h>
#include <sys/system_properties.h>

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace AndroidBridge {
namespace {

std::mutex g_pickMutex;
std::optional<std::pair<std::string, std::string>> g_picked;  // guarded by g_pickMutex

// Calls a static method of the activity's class (OtherMActivity: SDL's activity is the app's own subclass).
// GetObjectClass, not FindClass: a thread the game started sees only the system's classes through FindClass.
template <typename Call>
bool WithActivityClass(const char* name, const char* signature, Call call) {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    if (env == nullptr) return false;
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (activity == nullptr) return false;
    jclass type = env->GetObjectClass(activity);
    env->DeleteLocalRef(activity);
    jmethodID method = type != nullptr ? env->GetStaticMethodID(type, name, signature) : nullptr;
    if (method == nullptr) {
        env->ExceptionClear();
        if (type != nullptr) env->DeleteLocalRef(type);
        return false;
    }
    call(env, type, method);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    env->DeleteLocalRef(type);
    return true;
}

std::string Property(const char* name) {
    char value[PROP_VALUE_MAX] = {};
    return __system_property_get(name, value) > 0 ? std::string(value) : std::string();
}

} // namespace

void PickDisc() {
    {
        std::lock_guard lock(g_pickMutex);
        g_picked.reset();
    }
    WithActivityClass("pickDisc", "()V", [](JNIEnv* env, jclass type, jmethodID method) { env->CallStaticVoidMethod(type, method); });
}

bool TakePickedDisc(std::string& uri, std::string& name) {
    std::lock_guard lock(g_pickMutex);
    if (!g_picked) return false;
    uri = std::move(g_picked->first);
    name = std::move(g_picked->second);
    g_picked.reset();
    return true;
}

int OpenDocument(const std::string& uri) {
    int fd = -1;
    WithActivityClass("openDocument", "(Ljava/lang/String;)I", [&](JNIEnv* env, jclass type, jmethodID method) {
        jstring text = env->NewStringUTF(uri.c_str());
        fd = env->CallStaticIntMethod(type, method, text);
        env->DeleteLocalRef(text);
    });
    return fd;
}

void RestartApp() {
    WithActivityClass("restartApp", "()V", [](JNIEnv* env, jclass type, jmethodID method) { env->CallStaticVoidMethod(type, method); });
    // RestartActivity ends this process.
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}

void Quit() {
    WithActivityClass("quitApp", "()V", [](JNIEnv* env, jclass type, jmethodID method) { env->CallStaticVoidMethod(type, method); });
    // The activity's onDestroy ends the process; this is the fallback when it never comes.
    std::this_thread::sleep_for(std::chrono::seconds(2));
    std::_Exit(EXIT_SUCCESS);
}

int HttpGet(const std::string& url, const std::string& accept, std::string& body) {
    int status = -1;
    WithActivityClass("httpGet", "(Ljava/lang/String;Ljava/lang/String;[I)[B", [&](JNIEnv* env, jclass type, jmethodID method) {
        jstring jurl = env->NewStringUTF(url.c_str());
        jstring jaccept = env->NewStringUTF(accept.c_str());
        jintArray jstatus = env->NewIntArray(1);
        auto bytes = static_cast<jbyteArray>(env->CallStaticObjectMethod(type, method, jurl, jaccept, jstatus));
        if (bytes != nullptr) {
            const jsize length = env->GetArrayLength(bytes);
            body.resize(size_t(length));
            env->GetByteArrayRegion(bytes, 0, length, reinterpret_cast<jbyte*>(body.data()));
            env->GetIntArrayRegion(jstatus, 0, 1, &status);
            env->DeleteLocalRef(bytes);
        }
        env->DeleteLocalRef(jstatus);
        env->DeleteLocalRef(jaccept);
        env->DeleteLocalRef(jurl);
    });
    return status;
}

bool Download(const std::string& url, const std::string& path) {
    bool ok = false;
    WithActivityClass("download", "(Ljava/lang/String;Ljava/lang/String;)Z", [&](JNIEnv* env, jclass type, jmethodID method) {
        jstring jurl = env->NewStringUTF(url.c_str());
        jstring jpath = env->NewStringUTF(path.c_str());
        ok = env->CallStaticBooleanMethod(type, method, jurl, jpath) == JNI_TRUE;
        env->DeleteLocalRef(jpath);
        env->DeleteLocalRef(jurl);
    });
    return ok;
}

bool InstallPackage(const std::string& path) {
    bool ok = false;
    WithActivityClass("installPackage", "(Ljava/lang/String;)Z", [&](JNIEnv* env, jclass type, jmethodID method) {
        jstring jpath = env->NewStringUTF(path.c_str());
        ok = env->CallStaticBooleanMethod(type, method, jpath) == JNI_TRUE;
        env->DeleteLocalRef(jpath);
    });
    return ok;
}

std::string DeviceDescription() {
    std::string text = Property("ro.product.manufacturer");
    const std::string model = Property("ro.product.model");
    text += (text.empty() ? "" : " ") + model;
    // The chip: Android 12's ro.soc.* where the maker fills them in, else the board's platform name.
    std::string chip = Property("ro.soc.manufacturer");
    const std::string soc = Property("ro.soc.model");
    chip += (chip.empty() || soc.empty() ? "" : " ") + soc;
    if (chip.empty()) chip = Property("ro.board.platform");
    if (!chip.empty()) text += ", " + chip;
    text += ", Android " + Property("ro.build.version.release") + " (API " + Property("ro.build.version.sdk") + ")";
    return text;
}

} // namespace AndroidBridge

// aurora/lib/window.cpp: whether the drawing surface can be drawn on (its JNI name is for a patched SDLSurface.java;
// the app's own OtherMSurface reports instead).
extern "C" void Java_org_libsdl_app_SDLSurface_auroraNativeSetSurfaceReady(JNIEnv*, jclass, jboolean ready);

// OtherMSurface.nativeSetSurfaceReady: the surface was created or changed (true), or is going away (false).
extern "C" JNIEXPORT void JNICALL Java_io_github_gminustwo_otherm_OtherMSurface_nativeSetSurfaceReady(JNIEnv* env, jclass type,
                                                                                                    jboolean ready) {
    Java_org_libsdl_app_SDLSurface_auroraNativeSetSurfaceReady(env, type, ready);
}

// OtherMActivity.nativeDiscPicked: the picker's answer, on the UI thread.
extern "C" JNIEXPORT void JNICALL Java_io_github_gminustwo_otherm_OtherMActivity_nativeDiscPicked(JNIEnv* env, jclass, jstring uri,
                                                                                                jstring name) {
    const auto text = [env](jstring value) {
        std::string out;
        if (value == nullptr) return out;
        if (const char* utf8 = env->GetStringUTFChars(value, nullptr)) {
            out = utf8;
            env->ReleaseStringUTFChars(value, utf8);
        }
        return out;
    };
    std::lock_guard lock(AndroidBridge::g_pickMutex);
    AndroidBridge::g_picked.emplace(text(uri), text(name));
}
