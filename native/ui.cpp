#include <android/log.h>
#include <android/native_window.h>
#include <android/window.h>
#include <android_native_app_glue.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <openssl/crypto.h>

#include <algorithm>
#include <array>
#include <optional>
#include <string>

#include "camera.hpp"
#include "hex.hpp"
#include "wallet.hpp"
#include "wallet_store.hpp"

#include "imgui.h"
#include "imgui_impl_android.h"
#include "imgui_impl_opengl3.h"

#define LOG_TAG "Airgap"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace ui {
namespace {

EGLDisplay gDisplay = EGL_NO_DISPLAY;
EGLSurface gSurface = EGL_NO_SURFACE;
EGLContext gContext = EGL_NO_CONTEXT;

android_app* gApp = nullptr;

bool gInitialized = false;
bool gResumed = false;
std::optional<Camera> gCamera;
bool gCameraPermissionPending = false;
std::string gCameraMessage;

std::optional<WalletStore> gWalletStore;
std::optional<Wallet::Address> gActiveAddress;
std::optional<Wallet::Address> gPendingAddress;
Wallet::RecoveryWords gRecoveryWords{};
std::string gPendingAddressHex;
std::string gCreateMessage;
std::string gSelectMessage;

enum class BackupStage { None, FirstWarning, FinalWarning };
BackupStage gBackupStage = BackupStage::None;
bool gBackupReviewed = false;
bool gSelectWalletTab = false;

GLuint gCameraTexture = 0;
int gPreviewWidth = 0, gPreviewHeight = 0, gSensorOrientation = 0;
int gDisplayRotation = 0;

// NativeActivity has no Java source; use its existing Activity for permission/UI APIs.
struct ActivityJni {
    JavaVM* vm;
    JNIEnv* env = nullptr;
    bool attached = false;
    bool localFrame = false;

    explicit ActivityJni(ANativeActivity* activity) : vm(activity->vm) {
        const auto result = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);

        if (result == JNI_EDETACHED) {
            attached = vm->AttachCurrentThread(&env, nullptr) == JNI_OK;
            if (!attached)
                env = nullptr;

        } else if (result != JNI_OK) {
            env = nullptr;
        }

        if (env)
            localFrame = env->PushLocalFrame(16) == JNI_OK;
    }

    ~ActivityJni() {
        if (env && env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
        }

        if (localFrame)
            env->PopLocalFrame(nullptr);

        if (attached)
            vm->DetachCurrentThread();
    }
};

bool cameraPermission(bool request) {
    ActivityJni jni(gApp->activity);
    if (!jni.localFrame)
        return false;

    auto* env = jni.env;
    auto activity = gApp->activity->clazz;
    auto activityClass = env->GetObjectClass(activity);
    auto check = env->GetMethodID(activityClass, "checkSelfPermission", "(Ljava/lang/String;)I");
    if (!check)
        return false;

    auto permission = env->NewStringUTF("android.permission.CAMERA");
    if (!permission)
        return false;

    const auto granted = env->CallIntMethod(activity, check, permission);
    if (env->ExceptionCheck())
        return false;

    if (granted == 0)
        return true;

    if (request) {
        auto ask = env->GetMethodID(activityClass, "requestPermissions", "([Ljava/lang/String;I)V");
        if (!ask)
            return false;

        auto stringClass = env->FindClass("java/lang/String");
        if (!stringClass)
            return false;

        auto permissions = env->NewObjectArray(1, stringClass, permission);
        if (!permissions)
            return false;

        env->CallVoidMethod(activity, ask, permissions, 1);
    }

    return false;
}

void updateDisplayRotation() {
    ActivityJni jni(gApp->activity);
    if (!jni.localFrame)
        return;

    auto* env = jni.env;
    auto activity = gApp->activity->clazz;
    auto activityClass = env->GetObjectClass(activity);
    auto getManager =
        env->GetMethodID(activityClass, "getWindowManager", "()Landroid/view/WindowManager;");
    if (!getManager)
        return;

    auto manager = env->CallObjectMethod(activity, getManager);
    if (env->ExceptionCheck() || !manager)
        return;

    auto managerClass = env->GetObjectClass(manager);
    auto getDisplay =
        env->GetMethodID(managerClass, "getDefaultDisplay", "()Landroid/view/Display;");
    if (!getDisplay)
        return;

    auto display = env->CallObjectMethod(manager, getDisplay);
    if (env->ExceptionCheck() || !display)
        return;

    auto displayClass = env->GetObjectClass(display);
    auto getRotation = env->GetMethodID(displayClass, "getRotation", "()I");
    if (!getRotation)
        return;

    const int rotation = env->CallIntMethod(display, getRotation);
    if (!env->ExceptionCheck())
        gDisplayRotation = rotation * 90;
}

void closeCamera() {
    gCameraPermissionPending = false;

    if (gCamera)
        gCamera->close();

    if (gCameraTexture)
        glDeleteTextures(1, &gCameraTexture);

    gCameraTexture = 0;
    gPreviewWidth = gPreviewHeight = 0;
}

std::string addressHex(const Wallet::Address& address) {
    std::array<char, 43> text{};
    hex_encode(address.data(), address.size(), text.data());
    return text.data();
}

void clearRecoveryWords() {
    OPENSSL_cleanse(gRecoveryWords.data(), sizeof(gRecoveryWords));
    gPendingAddress.reset();
    gPendingAddressHex.clear();
    gBackupStage = BackupStage::None;
    gBackupReviewed = false;
}

void createStoredWallet() {
    gCreateMessage.clear();
    if (!gWalletStore)
        return;

    clearRecoveryWords();

    {
        auto wallet = Wallet::create(gApp->activity->assetManager,
                                     gRecoveryWords);
        if (!wallet) {
            gCreateMessage = "Wallet generation failed. Check the word list.";
            clearRecoveryWords();

            return;
        }

        gPendingAddress = wallet->address();
        gPendingAddressHex = wallet->addressHexEncoded();
        if (!gWalletStore->save(gPendingAddressHex, *wallet)) {
            gCreateMessage =
                "Wallet was not saved. TEE-backed Keystore or "
                "storage may be unavailable.";
            clearRecoveryWords();

            return;
        }
    }

    // The Wallet destructor above has already wiped the private key.
    gBackupStage = BackupStage::FirstWarning;
}

void selectStoredWallet(const WalletStore::WalletInfo& info) {
    gSelectMessage.clear();
    if (!gWalletStore)
        return;

    auto wallet = gWalletStore->load(info.name);
    if (!wallet) {
        gSelectMessage = "Could not verify or unlock this wallet.";
        return;
    }

    gActiveAddress = wallet->address();
}

void openCamera() {
    gCameraPermissionPending = false;
    if (gCamera && gCamera->open())
        gCameraMessage.clear();
    else
        gCameraMessage = gCamera ? gCamera->lastError() : "Camera manager unavailable";
}

// Move this call anywhere in the ImGui layout; Camera owns no UI/GL resources.
void drawCameraPreview() {
    if (!gCamera || !gResumed)
        return;

    if (gCameraPermissionPending && cameraPermission(false))
        openCamera();

    if (auto frame = gCamera->readFrame()) {
        if (!gCameraTexture) {
            glGenTextures(1, &gCameraTexture);
            glBindTexture(GL_TEXTURE_2D, gCameraTexture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }

        glBindTexture(GL_TEXTURE_2D, gCameraTexture);
        if (gPreviewWidth != frame->width || gPreviewHeight != frame->height) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, frame->width, frame->height, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, frame->rgba.data());

        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame->width, frame->height, GL_RGBA,
                            GL_UNSIGNED_BYTE, frame->rgba.data());
        }

        glBindTexture(GL_TEXTURE_2D, 0);
        gPreviewWidth = frame->width;
        gPreviewHeight = frame->height;
        gSensorOrientation = frame->sensorOrientation;
    }

    if (!gCamera->isOpen()) {
        if (!gCamera->lastError().empty())
            gCameraMessage = gCamera->lastError();

        if (gCameraTexture)
            closeCamera();

        return;
    }

    if (!gCameraTexture) {
        ImGui::TextUnformatted("Waiting for camera...");
        return;
    }

    // Back camera: sensor orientation minus current display rotation.
    const int turns = ((gSensorOrientation - gDisplayRotation + 360) % 360) / 90;
    const float width = (turns % 2) ? gPreviewHeight : gPreviewWidth;
    const float height = (turns % 2) ? gPreviewWidth : gPreviewHeight;
    const auto available = ImGui::GetContentRegionAvail();
    const float scale = std::min(available.x / width, available.y / height);

    if (scale <= 0)
        return;

    const ImVec2 size(width * scale, height * scale);
    const auto p = ImGui::GetCursorScreenPos();
    const ImVec2 uv[] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};

    ImGui::GetWindowDrawList()->AddImageQuad(
        static_cast<ImTextureID>(gCameraTexture), p, ImVec2(p.x + size.x, p.y),
        ImVec2(p.x + size.x, p.y + size.y), ImVec2(p.x, p.y + size.y), uv[(4 - turns) % 4],
        uv[(5 - turns) % 4], uv[(6 - turns) % 4], uv[(7 - turns) % 4]);

    ImGui::Dummy(size);
}

void drawRecoveryWords() {
    if (!ImGui::BeginTable("recovery words", 2))
        return;

    for (size_t i = 0; i < 12; ++i) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("%2zu. %s", i + 1, gRecoveryWords[i].data());

        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%2zu. %s", i + 13, gRecoveryWords[i + 12].data());
    }

    ImGui::EndTable();
}

void drawBackupScreen() {
    ImGui::TextUnformatted("Back up your new wallet");
    ImGui::TextWrapped("Address: %s", gPendingAddressHex.c_str());
    ImGui::Spacing();
    drawRecoveryWords();
    ImGui::Spacing();

    if (gBackupStage == BackupStage::FirstWarning) {
        ImGui::TextWrapped(
            "WARNING 1/2: Write all 24 words in order and keep them private. "
            "They are not saved and may be lost if the app closes. "
            "Anyone with them can take the funds in this wallet.");

        if (ImGui::Button("I wrote down all 24 words", ImVec2(-1, 110)))
            gBackupStage = BackupStage::FinalWarning;

    } else {
        ImGui::TextWrapped(
            "WARNING 2/2: These words will disappear after confirmation. "
            "Check your written copy now; you cannot reveal them again here.");

        ImGui::Checkbox("I checked every word in order", &gBackupReviewed);
        ImGui::BeginDisabled(!gBackupReviewed);

        if (ImGui::Button("BACKUP VERIFIED - HIDE WORDS", ImVec2(-1, 110))) {
            gActiveAddress = gPendingAddress;
            gSelectMessage.clear();
            clearRecoveryWords();
            gSelectWalletTab = true;
        }

        ImGui::EndDisabled();
    }
}

void drawCameraControls() {
    if (!gActiveAddress) {
        ImGui::TextWrapped("Select an active wallet before scanning a transaction.");
        return;
    }

    const char* label = gCamera && gCamera->isOpen() ?
                        "CLOSE CAMERA" : "SCAN TX";
    if (ImGui::Button(label, ImVec2(-1, 180))) {
        if (gCamera && gCamera->isOpen()) {
            closeCamera();
            gCameraMessage.clear();

        } else if (cameraPermission(true)) {
            openCamera();

        } else {
            gCameraPermissionPending = true;
            gCameraMessage =
                "Camera permission required. Allow access in the prompt or app settings.";
        }
    }

    drawCameraPreview();
    if (!gCameraMessage.empty())
        ImGui::TextWrapped("%s", gCameraMessage.c_str());
}

void drawSelectWalletMenu() {
    if (!gWalletStore) {
        ImGui::TextWrapped("Wallet store could not be opened. Check the app's private storage.");
        return;
    }

    if (gActiveAddress) {
        const std::string active = addressHex(*gActiveAddress);
        ImGui::TextUnformatted("Active wallet for this session:");
        ImGui::TextWrapped("%s", active.c_str());

    } else {
        ImGui::TextUnformatted("No active wallet selected.");
    }

    if (!gSelectMessage.empty())
        ImGui::TextWrapped("%s", gSelectMessage.c_str());

    ImGui::Separator();
    const auto wallets = gWalletStore->list();
    if (wallets.empty()) {
        ImGui::TextWrapped("No saved wallets. Open Create wallet to add one.");
        return;
    }

    if (ImGui::BeginChild("saved wallets", ImVec2(0, 600),
                          ImGuiChildFlags_Borders)) {
        for (size_t i = 0; i < wallets.size(); ++i) {
            const auto& info = wallets[i];
            const bool active = gActiveAddress == info.address;
            const std::string address = addressHex(info.address);
            ImGui::PushID(static_cast<int>(i));

            ImGui::TextWrapped("%s", address.c_str());
            if (ImGui::Button(active ? "ACTIVE" : "SELECT THIS WALLET",
                              ImVec2(-1, 80)) && !active)
                selectStoredWallet(info);

            ImGui::Separator();
            ImGui::PopID();
        }
    }

    ImGui::EndChild();
    ImGui::Spacing();
    drawCameraControls();
}

void drawCreateWalletMenu() {
    if (gCameraPermissionPending || (gCamera && gCamera->isOpen()))
        closeCamera();

    if (!gWalletStore) {
        ImGui::TextWrapped("Wallet store could not be opened. No wallet can be saved.");
        return;
    }

    ImGui::TextWrapped(
        "Create a wallet, protect its key with the TEE, then write down "
        "the 24 recovery words shown on the next screen.");

    if (ImGui::Button("CREATE AND SAVE WALLET", ImVec2(-1, 150)))
        createStoredWallet();

    if (!gCreateMessage.empty())
        ImGui::TextWrapped("%s", gCreateMessage.c_str());
}

bool initGraphics(android_app* app) {
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

    const EGLint configAttributes[] = {EGL_RED_SIZE,     8,
                                       EGL_GREEN_SIZE,   8,
                                       EGL_BLUE_SIZE,    8,
                                       EGL_ALPHA_SIZE,   8,

                                       EGL_DEPTH_SIZE,   0,

                                       EGL_SURFACE_TYPE, EGL_WINDOW_BIT,

                                       EGL_NONE};

    EGLConfig config = nullptr;
    EGLint configCount = 0;

    if (!eglChooseConfig(gDisplay, configAttributes, &config, 1, &configCount)) {
        LOGE("eglChooseConfig failed");
        return false;
    }

    if (configCount == 0) {
        LOGE("No EGL configuration found");
        return false;
    }

    EGLint format = 0;

    eglGetConfigAttrib(gDisplay, config, EGL_NATIVE_VISUAL_ID, &format);

    ANativeWindow_setBuffersGeometry(app->window, 0, 0, format);

    const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};

    gContext = eglCreateContext(gDisplay, config, EGL_NO_CONTEXT, contextAttributes);

    if (gContext == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed");
        return false;
    }

    gSurface = eglCreateWindowSurface(gDisplay, config, app->window, nullptr);

    if (gSurface == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface failed");
        return false;
    }

    if (!eglMakeCurrent(gDisplay, gSurface, gSurface, gContext)) {
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

    ImGui_ImplOpenGL3_Init("#version 300 es");

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

void shutdownGraphics() {
    closeCamera();
    if (!gInitialized)
        return;

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplAndroid_Shutdown();

    ImGui::DestroyContext();

    eglMakeCurrent(gDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

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

} // namespace

void drawFrame() {
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

    ImGui::SetNextWindowPos(ImVec2(0, io.DisplaySize.y * 0.1), ImGuiCond_Always);

    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, io.DisplaySize.y * 0.9), ImGuiCond_Always);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse;

    // Remove visible window border
    style.WindowBorderSize = 0.0f;

    ImGui::Begin("Airgap", nullptr, flags);

    ImGui::Text("Wallet");
    ImGui::Spacing();

    if (gBackupStage != BackupStage::None) {
        drawBackupScreen();

    } else if (ImGui::BeginTabBar("wallet menus")) {
        const auto selectFlags = gSelectWalletTab ?
                                 ImGuiTabItemFlags_SetSelected : 0;
        gSelectWalletTab = false;

        if (ImGui::BeginTabItem("Select wallet", nullptr, selectFlags)) {
            drawSelectWalletMenu();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Create wallet")) {
            drawCreateWalletMenu();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();

    //
    // Render
    //

    ImGui::Render();

    glViewport(0, 0, static_cast<int>(io.DisplaySize.x), static_cast<int>(io.DisplaySize.y));

    glClearColor(0.02f, 0.02f, 0.02f, 1.0f);

    glClear(GL_COLOR_BUFFER_BIT);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    eglSwapBuffers(gDisplay, gSurface);
}

void handleCommand(android_app* app, int32_t command) {
    switch (command) {

    case APP_CMD_INIT_WINDOW:
        LOGI("APP_CMD_INIT_WINDOW");

        initGraphics(app);
        updateDisplayRotation();
        break;

    case APP_CMD_RESUME:
        gResumed = true;
        break;

    case APP_CMD_PAUSE:
        gResumed = false;

        // Preserve a pending permission request while its dialog is visible.
        if (gCamera)
            gCamera->close();

        if (gCameraTexture) {
            const bool pending = gCameraPermissionPending;
            closeCamera();
            gCameraPermissionPending = pending;
        }

        break;

    case APP_CMD_STOP:
        closeCamera();
        break;

    case APP_CMD_CONFIG_CHANGED:
    case APP_CMD_WINDOW_RESIZED:
        updateDisplayRotation();
        break;

    case APP_CMD_TERM_WINDOW:
        LOGI("APP_CMD_TERM_WINDOW");

        shutdownGraphics();
        break;

    default:
        break;
    }
}

int32_t handleInput(android_app*, AInputEvent* event) {
    return ImGui_ImplAndroid_HandleInputEvent(event);
}

void initialize(android_app* app) {
    gApp = app;
    ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_SECURE, 0);
    gWalletStore = WalletStore::open(app->activity);
    gActiveAddress.reset();
    clearRecoveryWords();

    gCreateMessage.clear();
    gSelectMessage.clear();
    gSelectWalletTab = false;

    gCamera = Camera::init();
    if (!gCamera)
        gCameraMessage = "Camera manager unavailable";
}

void shutdown() {
    shutdownGraphics();
    clearRecoveryWords();
    gActiveAddress.reset();
    gWalletStore.reset();
    gCamera.reset();
}

bool isReady() {
    return gInitialized;
}

} // namespace ui
