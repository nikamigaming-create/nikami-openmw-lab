#include "androidpath.hpp"

#if defined(__ANDROID__)

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <jni.h>
#include <pwd.h>
#include <string>
#include <unistd.h>

static std::string g_path_global; //< Path to global directory root, e.g. /data/data/com.libopenmw.openmw
static std::string g_path_user; //< Path to user root, e.g. /sdcard/Android/data/com.libopenmw.openmw

static void setAndroidPaths(JNIEnv* env, jstring global, jstring user)
{
    const char* globalPath = env->GetStringUTFChars(global, nullptr);
    const char* userPath = env->GetStringUTFChars(user, nullptr);

    g_path_global = globalPath ? globalPath : "";
    g_path_user = userPath ? userPath : "";

    if (globalPath)
        env->ReleaseStringUTFChars(global, globalPath);
    if (userPath)
        env->ReleaseStringUTFChars(user, userPath);
}

extern "C" void openmw_set_android_paths(const char* global, const char* user)
{
    g_path_global = global ? global : "";
    g_path_user = user ? user : "";
}

static std::filesystem::path globalRoot()
{
    if (!g_path_global.empty())
        return g_path_global;

    return "/data/local/tmp/openmw-vrquest";
}

static std::filesystem::path userRoot()
{
    if (!g_path_user.empty())
        return g_path_user;

    return "/sdcard/Android/data/org.openmw.vrquest/files";
}

/**
 * \brief Called by java code to set up directory paths
 */
extern "C" JNIEXPORT void JNICALL Java_ui_activity_GameActivity_getPathToJni(
    JNIEnv* env, jobject obj, jstring global, jstring user)
{
    setAndroidPaths(env, global, user);
}

extern "C" JNIEXPORT void JNICALL Java_org_openmw_vrquest_OpenMWActivity_setOpenMWPaths(
    JNIEnv* env, jobject obj, jstring global, jstring user)
{
    setAndroidPaths(env, global, user);
}

namespace Files
{

    AndroidPath::AndroidPath(const std::string& application_name) {}

    // /sdcard/Android/data/com.libopenmw.openmw/config
    std::filesystem::path AndroidPath::getUserConfigPath() const
    {
        return userRoot() / "config";
    }

    // /sdcard/Android/data/com.libopenmw.openmw/
    // (so that saves are placed at /sdcard/Android/data/com.libopenmw.openmw/saves)
    std::filesystem::path AndroidPath::getUserDataPath() const
    {
        return userRoot();
    }

    // /data/data/com.libopenmw.openmw/cache
    // (supposed to be "official" android cache location)
    std::filesystem::path AndroidPath::getCachePath() const
    {
        return globalRoot() / "cache";
    }

    // /data/data/com.libopenmw.openmw/files/config
    // (note the addition of "files")
    std::filesystem::path AndroidPath::getGlobalConfigPath() const
    {
        return globalRoot() / "files" / "config";
    }

    std::filesystem::path AndroidPath::getLocalPath() const
    {
        return std::filesystem::path("./");
    }

    // /sdcard/Android/data/com.libopenmw.openmw
    // (so that the data is at /sdcard/Android/data/com.libopenmw.openmw/data)
    std::filesystem::path AndroidPath::getGlobalDataPath() const
    {
        return userRoot();
    }

    std::vector<std::filesystem::path> AndroidPath::getInstallPaths() const
    {
        return {};
    }

} /* namespace Files */

#endif /* defined(__Android__) */
