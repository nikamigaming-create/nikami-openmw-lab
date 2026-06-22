#if defined(__ANDROID__)

#include <android/log.h>
#include <android_native_app_glue.h>
#include <dlfcn.h>
#include <jni.h>
#include <SDL_events.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

extern "C" int SDL_main(int argc, char** argv);
extern "C" void openmw_set_android_paths(const char* global, const char* user);
extern "C" void openmw_initialize_openxr_loader(JavaVM* vm, JNIEnv* env, jobject activity);

namespace
{
    std::atomic_bool sActivityResumed = false;
    std::atomic_bool sHasFocus = false;
    std::atomic_bool sHasWindow = false;
    std::atomic_bool sSdlMainDone = false;
    std::atomic_bool sSdlQuitPushed = false;

    const char* appCommandName(int32_t cmd)
    {
        switch (cmd)
        {
            case APP_CMD_INPUT_CHANGED:
                return "APP_CMD_INPUT_CHANGED";
            case APP_CMD_INIT_WINDOW:
                return "APP_CMD_INIT_WINDOW";
            case APP_CMD_TERM_WINDOW:
                return "APP_CMD_TERM_WINDOW";
            case APP_CMD_WINDOW_RESIZED:
                return "APP_CMD_WINDOW_RESIZED";
            case APP_CMD_WINDOW_REDRAW_NEEDED:
                return "APP_CMD_WINDOW_REDRAW_NEEDED";
            case APP_CMD_CONTENT_RECT_CHANGED:
                return "APP_CMD_CONTENT_RECT_CHANGED";
            case APP_CMD_GAINED_FOCUS:
                return "APP_CMD_GAINED_FOCUS";
            case APP_CMD_LOST_FOCUS:
                return "APP_CMD_LOST_FOCUS";
            case APP_CMD_CONFIG_CHANGED:
                return "APP_CMD_CONFIG_CHANGED";
            case APP_CMD_LOW_MEMORY:
                return "APP_CMD_LOW_MEMORY";
            case APP_CMD_START:
                return "APP_CMD_START";
            case APP_CMD_RESUME:
                return "APP_CMD_RESUME";
            case APP_CMD_SAVE_STATE:
                return "APP_CMD_SAVE_STATE";
            case APP_CMD_PAUSE:
                return "APP_CMD_PAUSE";
            case APP_CMD_STOP:
                return "APP_CMD_STOP";
            case APP_CMD_DESTROY:
                return "APP_CMD_DESTROY";
            default:
                return "APP_CMD_UNKNOWN";
        }
    }

    void pushSdlQuit()
    {
        bool expected = false;
        if (!sSdlQuitPushed.compare_exchange_strong(expected, true))
            return;

        SDL_Event event{};
        event.type = SDL_QUIT;
        SDL_PushEvent(&event);
        __android_log_print(ANDROID_LOG_INFO, "OpenMWNative", "android_main: SDL_QUIT requested");
    }

    bool pumpAndroidEvents(android_app* app, int timeoutMs)
    {
        int events = 0;
        android_poll_source* source = nullptr;
        const int result = ALooper_pollOnce(timeoutMs, nullptr, &events, reinterpret_cast<void**>(&source));
        if (source)
            source->process(app, source);
        return result >= 0;
    }

    bool waitForLaunchReady(android_app* app)
    {
        constexpr int pollMs = 50;
        constexpr int maxPolls = 600;

        for (int i = 0; i < maxPolls; ++i)
        {
            if (app->destroyRequested)
            {
                __android_log_print(
                    ANDROID_LOG_WARN, "OpenMWNative", "android_main: launch gate aborting because destroy was requested");
                return false;
            }

            if (sHasWindow.load() && sActivityResumed.load())
                return true;

            if (i % 20 == 0)
            {
                __android_log_print(ANDROID_LOG_INFO, "OpenMWNative",
                    "android_main: launch gate waiting window=%d resumed=%d focus=%d destroy=%d",
                    sHasWindow.load() ? 1 : 0, sActivityResumed.load() ? 1 : 0, sHasFocus.load() ? 1 : 0,
                    app->destroyRequested ? 1 : 0);
            }

            pumpAndroidEvents(app, pollMs);
        }

        __android_log_print(ANDROID_LOG_WARN, "OpenMWNative",
            "android_main: launch gate timed out before SDL/OpenXR start window=%d resumed=%d focus=%d",
            sHasWindow.load() ? 1 : 0, sActivityResumed.load() ? 1 : 0, sHasFocus.load() ? 1 : 0);
        if (sHasWindow.load())
        {
            __android_log_print(ANDROID_LOG_WARN, "OpenMWNative",
                "android_main: proceeding with native window despite missing resumed/focus state");
            return true;
        }
        return false;
    }
}

static void setSdlNativeActivityWindow(ANativeWindow* window)
{
    using SetWindowFn = void (*)(ANativeWindow*);
    static SetWindowFn setWindow
        = reinterpret_cast<SetWindowFn>(dlsym(RTLD_DEFAULT, "SDL_Android_SetNativeActivityWindow"));
    if (setWindow)
        setWindow(window);
    else
        __android_log_print(ANDROID_LOG_WARN, "OpenMWNative", "SDL native window bridge not found");
}

static void handleAppCommand(android_app* app, int32_t cmd)
{
    __android_log_print(ANDROID_LOG_INFO, "OpenMWNative", "APP_CMD: %s (%d)", appCommandName(cmd), cmd);

    switch (cmd)
    {
        case APP_CMD_INIT_WINDOW:
            sHasWindow = app->window != nullptr;
            if (app->window)
                setSdlNativeActivityWindow(app->window);
            break;
        case APP_CMD_TERM_WINDOW:
            __android_log_print(ANDROID_LOG_INFO, "OpenMWNative", "APP_CMD_TERM_WINDOW deferred for VR handoff");
            break;
        case APP_CMD_RESUME:
            sActivityResumed = true;
            break;
        case APP_CMD_PAUSE:
        case APP_CMD_STOP:
            sActivityResumed = false;
            break;
        case APP_CMD_DESTROY:
            sActivityResumed = false;
            sHasFocus = false;
            sHasWindow = false;
            setSdlNativeActivityWindow(nullptr);
            pushSdlQuit();
            break;
        case APP_CMD_GAINED_FOCUS:
            sHasFocus = true;
            break;
        case APP_CMD_LOST_FOCUS:
            sHasFocus = false;
            break;
        default:
            break;
    }
}

void android_main(android_app* app)
{
    app_dummy();

    sActivityResumed = false;
    sHasFocus = false;
    sHasWindow = false;
    sSdlMainDone = false;
    sSdlQuitPushed = false;

    __android_log_print(ANDROID_LOG_INFO, "OpenMWNative", "android_main: begin");
    app->onAppCmd = handleAppCommand;

    for (int i = 0; !app->window && i < 500; ++i)
    {
        int events = 0;
        android_poll_source* source = nullptr;
        while (ALooper_pollOnce(20, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0)
        {
            if (source)
                source->process(app, source);
            if (app->window)
                break;
        }
    }

    if (app->window)
    {
        __android_log_print(ANDROID_LOG_INFO, "OpenMWNative", "android_main: native window ready %dx%d",
            ANativeWindow_getWidth(app->window), ANativeWindow_getHeight(app->window));
        setSdlNativeActivityWindow(app->window);
    }
    else
        __android_log_print(ANDROID_LOG_WARN, "OpenMWNative", "android_main: no native window before SDL_main");

    if (!waitForLaunchReady(app))
    {
        setSdlNativeActivityWindow(nullptr);
        return;
    }

    JNIEnv* env = nullptr;
    app->activity->vm->AttachCurrentThread(&env, nullptr);

    const char* root = "/sdcard/OpenMWVR";
    const char* user = "/sdcard/OpenMWVR/user";
    std::filesystem::create_directories("/sdcard/OpenMWVR/files/config");
    std::filesystem::create_directories("/sdcard/OpenMWVR/user/config");
    std::filesystem::create_directories("/sdcard/OpenMWVR/user/screenshots");
    openmw_set_android_paths(root, user);
    openmw_initialize_openxr_loader(app->activity->vm, env, app->activity->clazz);

    setenv("OPENMW_GLES_VERSION", "2", 1);
    setenv("OPENMW_DECOMPRESS_TEXTURES", "1", 1);
    setenv("LIBGL_GL", "21", 1);
    setenv("LIBGL_ES", "2", 1);
    setenv("LIBGL_LOGSHADERERROR", "1", 1);

    static char arg0[] = "openmw";
    static char skipMenu[] = "--skip-menu";
    static char start[] = "--start";
    static char startCell[] = "Goodsprings";
    static char* argv[] = { arg0, skipMenu, start, startCell };
    constexpr int argc = 4;

    __android_log_print(ANDROID_LOG_INFO, "OpenMWNative",
        "android_main: before SDL_main window=%d resumed=%d focus=%d", sHasWindow.load() ? 1 : 0,
        sActivityResumed.load() ? 1 : 0, sHasFocus.load() ? 1 : 0);

    __android_log_print(ANDROID_LOG_INFO, "OpenMWNative",
        "android_main: launching argv='%s %s %s %s'", arg0, skipMenu, start, startCell);

    std::thread sdlThread([]() {
        SDL_main(argc, argv);
        sSdlMainDone = true;
        __android_log_print(ANDROID_LOG_INFO, "OpenMWNative", "android_main: SDL_main returned");
    });

    int destroyPolls = 0;
    while (!sSdlMainDone)
    {
        if (app->destroyRequested)
        {
            pushSdlQuit();
            if (++destroyPolls > 40)
            {
                __android_log_print(ANDROID_LOG_WARN, "OpenMWNative",
                    "android_main: SDL_main did not exit after destroy request; returning native thread");
                break;
            }
        }

        int events = 0;
        android_poll_source* source = nullptr;
        while (ALooper_pollOnce(50, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0)
        {
            if (source)
                source->process(app, source);
            if (sSdlMainDone)
                break;
        }
    }

    if (sdlThread.joinable())
    {
        if (sSdlMainDone)
            sdlThread.join();
        else
            sdlThread.detach();
    }
    __android_log_print(ANDROID_LOG_INFO, "OpenMWNative", "android_main: after SDL_main");

    setSdlNativeActivityWindow(nullptr);
    app->activity->vm->DetachCurrentThread();
}

#endif
