#pragma once

#include <android_native_app_glue.h>

#include <cstdint>

namespace ui {

void initialize(android_app* app);
void shutdown();
bool isReady();
void drawFrame();
void handleCommand(android_app* app, int32_t command);
int32_t handleInput(android_app* app, AInputEvent* event);

} // namespace ui
