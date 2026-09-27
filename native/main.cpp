#include <android/log.h>
#include <android_native_app_glue.h>

#define LOG_TAG "Airgap"
#define LOGI(...) \
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

extern "C"
void android_main(android_app* app)
{
    app_dummy();

    LOGI("Hello World");
    LOGI("Native Home app started");

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