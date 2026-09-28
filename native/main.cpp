#include <android/log.h>
#include <android_native_app_glue.h>

#include "ui.hpp"

#define LOG_TAG "Airgap"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace {

void handleCommand(android_app* app, int32_t command) {
    ui::handleCommand(app, command);
}

int32_t handleInput(android_app* app, AInputEvent* event) {
    return ui::handleInput(app, event);
}

} // namespace

extern "C" void android_main(android_app* app) {
    app_dummy();

    LOGI("android_main");
    ui::initialize(app);

    app->onAppCmd = handleCommand;
    app->onInputEvent = handleInput;

    while (true) {
        int events = 0;
        android_poll_source* source = nullptr;

        // Poll without blocking while rendering; otherwise wait for Android events.
        while (ALooper_pollOnce(ui::isReady() ? 0 : -1, nullptr, &events,
                                reinterpret_cast<void**>(&source)) >= 0) {
            if (source)
                source->process(app, source);

            if (app->destroyRequested) {
                ui::shutdown();
                LOGI("Application destroyed");
                return;
            }
        }

        if (ui::isReady())
            ui::drawFrame();
    }
}
