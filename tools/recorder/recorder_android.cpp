// The motion recorder on Meta Quest (docs/recorder.md): NativeActivity through the NDK's native
// app glue, the Khronos loader initialized with the activity

#include "recorder.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <android/log.h>
#include <android_native_app_glue.h>
#include <jni.h>

#define XR_USE_PLATFORM_ANDROID
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

static void log_line (const char* line)
{
    __android_log_write(ANDROID_LOG_INFO, "puppetxr-recorder", line);
}

static bool make_directory (const char* path)
{
    return mkdir(path, 0770) == 0 || errno == EEXIST;
}

static bool session_directory (const char* root, char (&out)[RC_PATH_SIZE])
{
    const time_t now   = time(nullptr);
    struct tm    local = {};
    localtime_r(&now, &local);
    char takes[RC_PATH_SIZE];
    (void)snprintf(takes, sizeof(takes), "%s/takes", root);
    (void)snprintf(out, sizeof(out), "%s/%04d%02d%02d_%02d%02d%02d", takes, local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
    return make_directory(takes) && make_directory(out);
}

void android_main (android_app* app)
{
    JNIEnv* env = nullptr;
    app->activity->vm->AttachCurrentThread(&env, nullptr);

    PFN_xrInitializeLoaderKHR initialize_loader = nullptr;
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction*)&initialize_loader);
    const XrLoaderInitInfoAndroidKHR loader_info = { .type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR, .applicationVM = app->activity->vm, .applicationContext = app->activity->clazz };
    const bool loader_ready = initialize_loader && XR_SUCCEEDED(initialize_loader((const XrLoaderInitInfoBaseHeaderKHR*)&loader_info));

    char out_dir[RC_PATH_SIZE] = {};
    char protocol[RC_PATH_SIZE] = {};
    (void)snprintf(protocol, sizeof(protocol), "%s/protocol.json", app->activity->internalDataPath);
    const bool has_protocol = access(protocol, R_OK) == 0;
    const bool has_out      = session_directory(app->activity->externalDataPath, out_dir);

    const XrInstanceCreateInfoAndroidKHR android_info = { .type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR, .applicationVM = app->activity->vm, .applicationActivity = app->activity->clazz };
    RC_Options options = {
        .get_instance_proc_addr = (void*)&xrGetInstanceProcAddr,
        .instance_next          = &android_info,
        .extensions             = { XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME },
        .extensions_count       = 1,
        .out_dir                = out_dir,
        .protocol               = has_protocol ? protocol : nullptr,
        .space                  = RC_Space::LocalFloor,
        .passthrough            = true,
        .countdown_seconds      = 3.0f,
        .log                    = log_line,
    };
    if (!loader_ready || !has_out)
    {
        __android_log_print(ANDROID_LOG_ERROR, "puppetxr-recorder", "%s", !loader_ready ? "xrInitializeLoaderKHR failed" : "cannot create the takes directory");
    }
    RC_App*   recorder = loader_ready && has_out ? rc_create(options) : nullptr;
    RC_Status status   = recorder ? RC_Status::Running : RC_Status::Failed;
    if (recorder)
    {
        __android_log_print(ANDROID_LOG_INFO, "puppetxr-recorder", "takes go to %s%s", out_dir, has_protocol ? ", protocol.json found" : "");
    }
    else
    {
        ANativeActivity_finish(app->activity);
    }

    while (!app->destroyRequested)
    {
        int                  events = 0;
        android_poll_source* source = nullptr;
        // Drains the system's events; waits for the first only while no session runs
        for (int timeout = status == RC_Status::Running ? 0 : -1; ALooper_pollOnce(timeout, nullptr, &events, (void**)&source) >= 0 && !app->destroyRequested; timeout = 0)
        {
            if (source)
            {
                source->process(app, source);
            }
        }
        if (status == RC_Status::Running)
        {
            status = rc_frame(recorder);
            if (status != RC_Status::Running)
            {
                ANativeActivity_finish(app->activity);
            }
        }
    }
    rc_destroy(recorder);
    app->activity->vm->DetachCurrentThread();
}
