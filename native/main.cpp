#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>

#define LOG_TAG "Airgap"
#define LOGI(...) \
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

static void draw(android_app* app)
{
    if (!app->window)
        return;

    ANativeWindow_setBuffersGeometry(
        app->window,
        0,
        0,
        WINDOW_FORMAT_RGBA_8888
    );

    ANativeWindow_Buffer buffer{};

    if (ANativeWindow_lock(
            app->window,
            &buffer,
            nullptr) != 0)
    {
        return;
    }

    auto* pixels =
        static_cast<uint32_t*>(buffer.bits);

    for (int y = 0; y < buffer.height; ++y) {
        auto* row = pixels + y * buffer.stride;

        for (int x = 0; x < buffer.width; ++x) {
            row[x] = 0xFF00FF00;
        }
    }

    ANativeWindow_unlockAndPost(app->window);
}

static void onCommand(android_app* app, int32_t cmd)
{
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            LOGI("Window initialized");
            draw(app);
            break;

        case APP_CMD_WINDOW_REDRAW_NEEDED:
            draw(app);
            break;
    }
}

extern "C"
void android_main(android_app* app)
{
    app_dummy();

    LOGI("Native Home app started");

    app->onAppCmd = onCommand;

    while (true) {
        int events = 0;
        android_poll_source* source = nullptr;

        ALooper_pollOnce(
            -1,
            nullptr,
            &events,
            reinterpret_cast<void**>(&source)
        );

        if (source)
            source->process(app, source);

        if (app->destroyRequested)
            return;
    }
}