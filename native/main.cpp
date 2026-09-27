#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include "imgui.h"
#include "imgui_impl_android.h"
#include "imgui_impl_opengl3.h"

#define LOG_TAG "Airgap"

#define LOGI(...) \
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

#define LOGE(...) \
    __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

EGLDisplay gDisplay = EGL_NO_DISPLAY;
EGLSurface gSurface = EGL_NO_SURFACE;
EGLContext gContext = EGL_NO_CONTEXT;

android_app* gApp = nullptr;

bool gInitialized = false;

bool initGraphics(android_app* app)
{
    if (gInitialized)
        return true;

    if (!app->window)
        return false;

    gApp = app;

    gDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);

    if (gDisplay == EGL_NO_DISPLAY) {
        LOGE("eglGetDisplay failed");
        return false;
    }

    if (!eglInitialize(gDisplay, nullptr, nullptr)) {
        LOGE("eglInitialize failed");
        return false;
    }

    const EGLint configAttributes[] = {
        EGL_RED_SIZE,     8,
        EGL_GREEN_SIZE,   8,
        EGL_BLUE_SIZE,    8,
        EGL_ALPHA_SIZE,   8,

        EGL_DEPTH_SIZE,   0,

        EGL_SURFACE_TYPE,
        EGL_WINDOW_BIT,

        EGL_NONE
    };

    EGLConfig config = nullptr;
    EGLint configCount = 0;

    if (!eglChooseConfig(
            gDisplay,
            configAttributes,
            &config,
            1,
            &configCount))
    {
        LOGE("eglChooseConfig failed");
        return false;
    }

    if (configCount == 0) {
        LOGE("No EGL configuration found");
        return false;
    }

    EGLint format = 0;

    eglGetConfigAttrib(
        gDisplay,
        config,
        EGL_NATIVE_VISUAL_ID,
        &format
    );

    ANativeWindow_setBuffersGeometry(
        app->window,
        0,
        0,
        format
    );

    const EGLint contextAttributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    };

    gContext = eglCreateContext(
        gDisplay,
        config,
        EGL_NO_CONTEXT,
        contextAttributes
    );

    if (gContext == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed");
        return false;
    }

    gSurface = eglCreateWindowSurface(
        gDisplay,
        config,
        app->window,
        nullptr
    );

    if (gSurface == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface failed");
        return false;
    }

    if (!eglMakeCurrent(
            gDisplay,
            gSurface,
            gSurface,
            gContext))
    {
        LOGE("eglMakeCurrent failed");
        return false;
    }

    //
    // Dear ImGui
    //

    IMGUI_CHECKVERSION();

    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();

    // We don't really need imgui.ini for a fixed wallet UI.
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();

    ImGui_ImplAndroid_Init(app->window);

    ImGui_ImplOpenGL3_Init(
        "#version 300 es"
    );

    //
    // Scale UI for a phone screen.
    //
    // You will probably tune this later.
    //

    ImGuiStyle& style = ImGui::GetStyle();

    style.ScaleAllSizes(3.5f);
    style.FontScaleDpi = 3.5f;

    gInitialized = true;

    LOGI("ImGui initialized");

    return true;
}

void shutdownGraphics()
{
    if (!gInitialized)
        return;

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplAndroid_Shutdown();

    ImGui::DestroyContext();

    eglMakeCurrent(
        gDisplay,
        EGL_NO_SURFACE,
        EGL_NO_SURFACE,
        EGL_NO_CONTEXT
    );

    if (gContext != EGL_NO_CONTEXT)
        eglDestroyContext(gDisplay, gContext);

    if (gSurface != EGL_NO_SURFACE)
        eglDestroySurface(gDisplay, gSurface);

    if (gDisplay != EGL_NO_DISPLAY)
        eglTerminate(gDisplay);

    gDisplay = EGL_NO_DISPLAY;
    gSurface = EGL_NO_SURFACE;
    gContext = EGL_NO_CONTEXT;

    gInitialized = false;

    LOGI("ImGui shutdown");
}

void drawFrame()
{
    if (!gInitialized)
        return;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame();

    ImGui::NewFrame();

    //
    // UI starts here.
    //

    ImGuiIO& io = ImGui::GetIO();
    ImGuiStyle& style = ImGui::GetStyle();

    ImGui::SetNextWindowPos(
        ImVec2(0, io.DisplaySize.y * 0.1),
        ImGuiCond_Always
    );

    ImGui::SetNextWindowSize(
        ImVec2(io.DisplaySize.x, io.DisplaySize.y * 0.9),
        ImGuiCond_Always
    );

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse;

    // Remove visible window border
    style.WindowBorderSize = 0.0f;

    ImGui::Begin(
        "Airgap",
        nullptr,
        flags
    );

    ImGui::Text("airgap wallet");

    ImGui::Spacing();
    ImGui::Spacing();

    ImGui::Text(
        "Ethereum cold wallet"
    );

    ImGui::Spacing();

    if (ImGui::Button(
            "SCAN TRANSACTION",
            ImVec2(-1, 180)))
    {
        LOGI(
            "Scan transaction clicked"
        );
    }

    ImGui::End();

    //
    // Render
    //

    ImGui::Render();

    glViewport(
        0,
        0,
        static_cast<int>(
            io.DisplaySize.x),
        static_cast<int>(
            io.DisplaySize.y)
    );

    glClearColor(
        0.02f,
        0.02f,
        0.02f,
        1.0f
    );

    glClear(
        GL_COLOR_BUFFER_BIT
    );

    ImGui_ImplOpenGL3_RenderDrawData(
        ImGui::GetDrawData()
    );

    eglSwapBuffers(
        gDisplay,
        gSurface
    );
}

void handleCommand(
    android_app* app,
    int32_t command)
{
    switch (command) {

        case APP_CMD_INIT_WINDOW:
            LOGI(
                "APP_CMD_INIT_WINDOW"
            );

            initGraphics(app);
            break;

        case APP_CMD_TERM_WINDOW:
            LOGI(
                "APP_CMD_TERM_WINDOW"
            );

            shutdownGraphics();
            break;

        default:
            break;
    }
}

int32_t handleInput(
    android_app*,
    AInputEvent* event)
{
    return
        ImGui_ImplAndroid_HandleInputEvent(
            event
        );
}

} // namespace

extern "C"
void android_main(android_app* app)
{
    app_dummy();

    LOGI("android_main");

    app->onAppCmd =
        handleCommand;

    app->onInputEvent =
        handleInput;

    while (true) {

        int events = 0;

        android_poll_source* source =
            nullptr;

        //
        // When rendering, poll without blocking.
        // Before we have a window, block waiting
        // for Android events.
        //

        while (
            ALooper_pollOnce(
                gInitialized ? 0 : -1,
                nullptr,
                &events,
                reinterpret_cast<void**>(
                    &source
                )
            ) >= 0)
        {
            if (source)
                source->process(
                    app,
                    source
                );

            if (app->destroyRequested) {

                shutdownGraphics();

                LOGI(
                    "Application destroyed"
                );

                return;
            }
        }

        if (gInitialized)
            drawFrame();
    }
}