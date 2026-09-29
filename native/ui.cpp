// Airgap wallet UI: one full-screen Dear ImGui window driven by android_native_app_glue.

// Index of this file:
// [SECTION] Includes, macros
// [SECTION] Types, constants
// [SECTION] State
// [SECTION] Android helpers (JNI)
// [SECTION] Formatting helpers
// [SECTION] Widgets
// [SECTION] Camera, QR scanning
// [SECTION] Wallets
// [SECTION] Transactions
// [SECTION] Known calls
// [SECTION] Main window
// [SECTION] Graphics lifecycle
// [SECTION] Public API

//-----------------------------------------------------------------------------
// [SECTION] Includes, macros
//-----------------------------------------------------------------------------

#include <android/log.h>
#include <android/native_window.h>
#include <android/window.h>
#include <android_native_app_glue.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <openssl/crypto.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <memory>
#include <utility>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "call_definitions.hpp"
#include "call_definitions_storage.hpp"
#include "camera.hpp"
#include "hex.hpp"
#include "qr_scan_worker.hpp"

#include "types.hpp"
#include "wallet.hpp"
#include "wallet_store.hpp"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"
#include "imgui_impl_android.h"
#include "imgui_impl_opengl3.h"

#define LOG_TAG "Airgap"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace ui {
namespace {

//-----------------------------------------------------------------------------
// [SECTION] Types, constants
//-----------------------------------------------------------------------------

enum class Screen { Home, Wallets, CreateWallet, Backup, Scan, Review, Signature, KnownCalls };
enum class BackupStage { FirstWarning, FinalWarning };
enum class ButtonStyle { Normal, Primary, Danger };

struct Chain {
    uint64_t id;
    const char* name;
    const char* symbol;
};

constexpr Chain kChains[] = {
    {1, "Ethereum", "ETH"},
    {10, "OP Mainnet", "ETH"},
    {56, "BNB Smart Chain", "BNB"},
    {137, "Polygon", "POL"},
    {8453, "Base", "ETH"},
    {42161, "Arbitrum One", "ETH"},

    {43114, "Avalanche C-Chain", "AVAX"},

    // Testnets
    {17000, "Holesky testnet", "ETH"},
    {11155111, "Sepolia testnet", "ETH"},
};

// One declaration in the known calls text, by its line index in that text.
struct KnownCall {
    size_t line;
    std::string_view text;
};

// Sizes are in pixels after the 3.5x phone scale.
constexpr float kButtonHeight = 140.0f;
constexpr float kRowButtonHeight = 100.0f;
constexpr float kUiScale = 3.5f;

// Touch scrolling: glide slows by this factor per second and stops below kMinGlide px/s.
constexpr float kGlideFriction = 4.0f;
constexpr float kMinGlide = 60.0f;

constexpr ImVec4 kPrimaryColors[] = {{0.16f, 0.45f, 0.86f, 1.0f},
                                     {0.22f, 0.52f, 0.93f, 1.0f},
                                     {0.12f, 0.38f, 0.76f, 1.0f}};

constexpr ImVec4 kDangerColors[] = {{0.68f, 0.21f, 0.21f, 1.0f},
                                    {0.78f, 0.27f, 0.27f, 1.0f},
                                    {0.58f, 0.16f, 0.16f, 1.0f}};

constexpr ImVec4 kNoticeColor = {0.95f, 0.80f, 0.35f, 1.0f};
constexpr ImVec4 kWarningColor = {1.00f, 0.60f, 0.30f, 1.0f};

//-----------------------------------------------------------------------------
// [SECTION] State
//-----------------------------------------------------------------------------

// Graphics, platform
EGLDisplay gDisplay = EGL_NO_DISPLAY;
EGLSurface gSurface = EGL_NO_SURFACE;
EGLContext gContext = EGL_NO_CONTEXT;

android_app* gApp = nullptr;
bool gInitialized = false;
bool gResumed = false;
bool gKeyboardRequested = false;

// Navigation; screen changes are applied at the start of the next frame.
Screen gScreen = Screen::Home;
std::optional<Screen> gNextScreen;
std::string gNextMessage;
std::string gMessage;
ImGuiID gArmedButton = 0;

// Touch scrolling; velocity is in scroll pixels per second and non-zero while gliding.
bool gTouchScrolling = false;
float gScrollStartY = 0.0f;
float gLastDragY = 0.0f;
float gScrollVelocity = 0.0f;

// Camera, QR scanning
std::optional<Camera> gCamera;
std::unique_ptr<QrScanWorker> gQrWorker;
bool gCameraPermissionPending = false;
GLuint gCameraTexture = 0;
int gPreviewWidth = 0, gPreviewHeight = 0, gSensorOrientation = 0;
int gDisplayRotation = 0;

// Wallets
std::optional<WalletStore> gWalletStore;
std::optional<WalletStore::WalletInfo> gActiveWallet;
std::optional<WalletStore::WalletInfo> gPendingWallet;
Wallet::RecoveryWords gRecoveryWords{};
BackupStage gBackupStage = BackupStage::FirstWarning;
bool gBackupReviewed = false;

// Transactions
std::optional<QrScanWorker::Result> gScanResult;
std::optional<Wallet::Signature> gSignature;
bool gReviewConfirmed = false;

// Known calls
std::optional<CallDefinitions> gCallDefinitions;
std::string gCallText;
std::string gCallInput;

//-----------------------------------------------------------------------------
// [SECTION] Android helpers (JNI)
//-----------------------------------------------------------------------------

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

bool showSoftKeyboard() {
    ActivityJni jni(gApp->activity);
    if (!jni.localFrame)
        return false;

    auto* env = jni.env;
    auto activity = gApp->activity->clazz;
    auto activityClass = env->GetObjectClass(activity);
    auto getCurrentFocus = env->GetMethodID(activityClass, "getCurrentFocus",
                                             "()Landroid/view/View;");
    if (!getCurrentFocus)
        return false;

    auto focusedView = env->CallObjectMethod(activity, getCurrentFocus);
    if (env->ExceptionCheck())
        return false;

    auto getWindow = env->GetMethodID(activityClass, "getWindow", "()Landroid/view/Window;");
    if (!getWindow)
        return false;

    auto window = env->CallObjectMethod(activity, getWindow);
    if (env->ExceptionCheck() || !window)
        return false;

    auto windowClass = env->GetObjectClass(window);
    auto getDecorView = env->GetMethodID(windowClass, "getDecorView", "()Landroid/view/View;");
    if (!getDecorView)
        return false;

    auto decorView = env->CallObjectMethod(window, getDecorView);
    if (env->ExceptionCheck() || !decorView)
        return false;

    auto inputView = focusedView ? focusedView : decorView;

    auto inputMethodClass = env->FindClass("android/view/inputmethod/InputMethodManager");
    if (!inputMethodClass)
        return false;

    auto getSystemService = env->GetMethodID(
        activityClass, "getSystemService", "(Ljava/lang/Class;)Ljava/lang/Object;");
    if (!getSystemService)
        return false;

    auto inputMethod = env->CallObjectMethod(activity, getSystemService, inputMethodClass);
    if (env->ExceptionCheck() || !inputMethod)
        return false;

    auto showSoftInput = env->GetMethodID(inputMethodClass, "showSoftInput",
                                           "(Landroid/view/View;I)Z");
    if (!showSoftInput)
        return false;

    const jboolean requested = env->CallBooleanMethod(inputMethod, showSoftInput,
                                                       inputView, 0);
    return !env->ExceptionCheck() && requested;
}

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

void addTextInput(AInputEvent* event) {
    if (!gInitialized || AInputEvent_getType(event) != AINPUT_EVENT_TYPE_KEY ||
        AKeyEvent_getAction(event) != AKEY_EVENT_ACTION_DOWN)
        return;

    ActivityJni jni(gApp->activity);
    if (!jni.localFrame)
        return;

    JNIEnv* env = jni.env;
    jclass type = env->FindClass("android/view/KeyEvent");
    if (!type)
        return;

    jmethodID constructor = env->GetMethodID(type, "<init>", "(II)V");
    jmethodID getUnicode = env->GetMethodID(type, "getUnicodeChar", "(I)I");
    if (!constructor || !getUnicode)
        return;

    jobject key = env->NewObject(type, constructor, AKEY_EVENT_ACTION_DOWN,
                                  AKeyEvent_getKeyCode(event));
    if (!key || env->ExceptionCheck())
        return;

    const jint codepoint = env->CallIntMethod(
        key, getUnicode, AKeyEvent_getMetaState(event));
    if (!env->ExceptionCheck() && codepoint >= 32)
        ImGui::GetIO().AddInputCharacter(static_cast<unsigned int>(codepoint));
}

void updateSoftKeyboard() {
    const bool wantsKeyboard = ImGui::GetIO().WantTextInput;

    if (wantsKeyboard && !gKeyboardRequested) {
        gKeyboardRequested = showSoftKeyboard();

    } else if (!wantsKeyboard && gKeyboardRequested) {
        ANativeActivity_hideSoftInput(gApp->activity, 0);
        gKeyboardRequested = false;
    }
}

//-----------------------------------------------------------------------------
// [SECTION] Formatting helpers
//-----------------------------------------------------------------------------

std::string hexString(const uint8_t* bytes, size_t size) {
    std::string text(2 * size + 3, '\0');
    hex_encode(bytes, size, text.data());
    text.resize(2 * size + 2);

    return text;
}

template <size_t N>
std::string hexString(const std::array<uint8_t, N>& bytes) {
    return hexString(bytes.data(), bytes.size());
}

// Big-endian unsigned integer to base-10 digits.
std::string decimalString(const eth::Uint256& value) {
    std::string digits = "0";

    for (uint8_t byte : value) {
        unsigned carry = byte;

        for (char& digit : digits) {
            carry += static_cast<unsigned>(digit - '0') * 256;
            digit = static_cast<char>('0' + carry % 10);
            carry /= 10;
        }

        while (carry != 0) {
            digits.push_back(static_cast<char>('0' + carry % 10));
            carry /= 10;
        }
    }

    return std::string(digits.rbegin(), digits.rend());
}

// Formats base units with a decimal point, e.g. 1500000000 at 9 decimals is "1.5".
std::string formatUnits(const eth::Uint256& value, size_t decimals) {
    std::string digits = decimalString(value);
    if (digits.size() <= decimals)
        digits.insert(0, decimals - digits.size() + 1, '0');

    const size_t point = digits.size() - decimals;
    std::string fraction = digits.substr(point);
    while (!fraction.empty() && fraction.back() == '0')
        fraction.pop_back();

    const std::string whole = digits.substr(0, point);
    return fraction.empty() ? whole : whole + "." + fraction;
}

const Chain* findChain(uint64_t id) {
    for (const Chain& chain : kChains) {
        if (chain.id == id)
            return &chain;
    }

    return nullptr;
}

std::string_view trim(std::string_view text) {
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return {};

    const size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::vector<std::string_view> splitLines(std::string_view text) {
    std::vector<std::string_view> lines;

    while (!text.empty()) {
        const size_t end = text.find('\n');
        lines.push_back(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    }

    return lines;
}

bool isIdentifierChar(char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_';
}

bool functionKeywordAt(std::string_view text, size_t offset) {
    constexpr std::string_view keyword = "function";
    if (text.substr(offset, keyword.size()) != keyword)
        return false;

    const size_t end = offset + keyword.size();
    const bool startsWord = offset == 0 || !isIdentifierChar(text[offset - 1]);
    const bool endsWord = end == text.size() || !isIdentifierChar(text[end]);

    return startsWord && endsWord;
}

// Splits "function a(...) function b(...)" into one single-line declaration each.
std::vector<std::string> splitDeclarations(std::string_view text) {
    std::vector<std::string> declarations;
    size_t start = 0;

    for (size_t i = 1; i <= text.size(); ++i) {
        if (i < text.size() && !functionKeywordAt(text, i))
            continue;

        std::string declaration(trim(text.substr(start, i - start)));
        std::replace_if(declaration.begin(), declaration.end(),
                        [](char value) { return value == '\n' || value == '\r' || value == '\t'; },
                        ' ');

        while (!declaration.empty() && (declaration.back() == ';' || declaration.back() == ' '))
            declaration.pop_back();

        if (!declaration.empty())
            declarations.push_back(std::move(declaration));

        start = i;
    }

    return declarations;
}

//-----------------------------------------------------------------------------
// [SECTION] Widgets
//-----------------------------------------------------------------------------

// Requests a screen change; it is applied at the start of the next frame.
void openScreen(Screen screen, std::string message = {}) {
    gNextScreen = screen;
    gNextMessage = std::move(message);
}

bool button(const char* label, ButtonStyle style = ButtonStyle::Normal,
            float height = kButtonHeight) {
    const ImVec4* colors = style == ButtonStyle::Primary ? kPrimaryColors :
                           style == ButtonStyle::Danger  ? kDangerColors : nullptr;

    if (colors) {
        ImGui::PushStyleColor(ImGuiCol_Button, colors[0]);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors[1]);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors[2]);
    }

    const bool pressed = ImGui::Button(label, ImVec2(-1, height));

    if (colors)
        ImGui::PopStyleColor(3);

    return pressed;
}

// First tap arms the button, a second tap on the same button confirms.
bool confirmButton(const char* label, ButtonStyle style = ButtonStyle::Normal,
                   float height = kButtonHeight) {
    const ImGuiID id = ImGui::GetID(label);
    const bool armed = gArmedButton == id;
    const std::string text = armed ? std::string("TAP AGAIN TO CONFIRM###") + label : label;

    if (!button(text.c_str(), armed ? ButtonStyle::Danger : style, height))
        return false;

    gArmedButton = armed ? 0 : id;
    return armed;
}

void coloredText(const ImVec4& color, const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

void hintText(const char* text) {
    coloredText(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), text);
}

void warningText(const char* text) {
    coloredText(kWarningColor, text);
}

void bulletText(const char* text) {
    ImGui::Bullet();
    ImGui::SameLine();
    ImGui::TextWrapped("%s", text);
}

void sectionLabel(const char* text) {
    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::TextDisabled("%s", text);
}

void drawMessage() {
    if (gMessage.empty())
        return;

    coloredText(kNoticeColor, gMessage.c_str());
    ImGui::Spacing();
}

void drawHeader(const char* title, bool canGoBack = true) {
    if (canGoBack) {
        if (ImGui::Button("< BACK"))
            openScreen(Screen::Home);

        ImGui::SameLine();
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(title);
    ImGui::Separator();
    ImGui::Spacing();

    drawMessage();
}

// Wraps ImGui::BeginTable for label/value rows; call ImGui::EndTable() when true.
bool beginDetails(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_BordersOuter))
        return false;

    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);

    return true;
}

void detailRow(std::string_view label, std::string_view value) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(label.data(), label.data() + label.size());
    ImGui::PopStyleColor();

    ImGui::TableSetColumnIndex(1);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(value.data(), value.data() + value.size());
    ImGui::PopTextWrapPos();
}

//-----------------------------------------------------------------------------
// [SECTION] Camera, QR scanning
//-----------------------------------------------------------------------------

void closeCamera() {
    gCameraPermissionPending = false;
    gQrWorker.reset();

    if (gCamera)
        gCamera->close();

    if (gCameraTexture)
        glDeleteTextures(1, &gCameraTexture);

    gCameraTexture = 0;
    gPreviewWidth = gPreviewHeight = 0;
}

void openCamera() {
    gCameraPermissionPending = false;

    if (!gCamera || !gCamera->open()) {
        gQrWorker.reset();
        gMessage = gCamera ? gCamera->lastError() : "Camera manager unavailable.";
        return;
    }

    try {
        if (!gQrWorker)
            gQrWorker = std::make_unique<QrScanWorker>();

        gMessage.clear();

    } catch (const std::exception&) {
        closeCamera();
        gMessage = "Could not start QR scanning. Please try again.";
    }
}

bool cameraActive() {
    return gCameraPermissionPending || (gCamera && gCamera->isOpen());
}

// Returns true once a QR code has been decoded into gScanResult.
bool collectScanResult() {
    if (!gQrWorker)
        return false;

    auto completion = gQrWorker->takeResult();
    if (!completion)
        return false;

    if (!completion->error.empty()) {
        gMessage = std::move(completion->error);
        closeCamera();
        return false;
    }

    if (!completion->result)
        return false;

    gScanResult = std::move(completion->result);
    closeCamera();

    return true;
}

// Uploads the newest camera frame and draws it rotated to the display orientation.
void drawCameraPreview() {
    if (!gCamera || !gResumed)
        return;

    if (gCameraPermissionPending) {
        if (!cameraPermission(false))
            return;

        openCamera();
    }

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

        if (gQrWorker)
            gQrWorker->submit(std::move(frame->rgba), frame->width, frame->height);
    }

    if (!gCamera->isOpen()) {
        if (!gCamera->lastError().empty())
            gMessage = gCamera->lastError();

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

//-----------------------------------------------------------------------------
// [SECTION] Wallets
//-----------------------------------------------------------------------------

void clearRecoveryWords() {
    OPENSSL_cleanse(gRecoveryWords.data(), sizeof(gRecoveryWords));
    gPendingWallet.reset();
    gBackupStage = BackupStage::FirstWarning;
    gBackupReviewed = false;
}

void createStoredWallet() {
    if (!gWalletStore)
        return;

    clearRecoveryWords();

    {
        auto wallet = Wallet::create(gApp->activity->assetManager, gRecoveryWords);
        if (!wallet) {
            gMessage = "Wallet generation failed. Check the word list.";
            clearRecoveryWords();
            return;
        }

        WalletStore::WalletInfo info{wallet->addressHexEncoded(), wallet->address()};
        if (!gWalletStore->save(info.name, *wallet)) {
            gMessage = "Wallet was not saved. TEE-backed Keystore or storage may be unavailable.";
            clearRecoveryWords();
            return;
        }

        gPendingWallet = std::move(info);
    }

    // The Wallet destructor above has already wiped the private key.
    openScreen(Screen::Backup);
}

void confirmBackup() {
    gActiveWallet = gPendingWallet;
    clearRecoveryWords();

    openScreen(Screen::Home, "Wallet created and set as the active wallet.");
}

void selectStoredWallet(const WalletStore::WalletInfo& info) {
    if (!gWalletStore)
        return;

    // Unlocking once proves the Keystore key still decrypts this wallet.
    if (!gWalletStore->load(info.name)) {
        gMessage = "Could not verify or unlock this wallet.";
        return;
    }

    gActiveWallet = info;
    openScreen(Screen::Home, "Active wallet changed.");
}

void drawWalletsScreen() {
    drawHeader("Wallets");

    if (!gWalletStore) {
        ImGui::TextWrapped("Wallet storage could not be opened. Check the app's private storage.");
        return;
    }

    const auto wallets = gWalletStore->list();
    if (wallets.empty()) {
        ImGui::TextWrapped("No wallets yet.");

        if (button("CREATE WALLET", ButtonStyle::Primary))
            openScreen(Screen::CreateWallet);

        return;
    }

    hintText("The active wallet signs every new transaction.");

    for (size_t i = 0; i < wallets.size(); ++i) {
        const auto& info = wallets[i];
        const bool active = gActiveWallet && gActiveWallet->address == info.address;
        const auto flags = ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY;

        ImGui::PushID(static_cast<int>(i));

        if (ImGui::BeginChild("wallet", ImVec2(0, 0), flags)) {
            ImGui::Text("Wallet %zu%s", i + 1, active ? "  -  ACTIVE" : "");
            ImGui::TextWrapped("%s", hexString(info.address).c_str());

            ImGui::BeginDisabled(active);
            const auto style = active ? ButtonStyle::Normal : ButtonStyle::Primary;
            if (button(active ? "ACTIVE" : "USE THIS WALLET", style, kRowButtonHeight))
                selectStoredWallet(info);

            ImGui::EndDisabled();
        }

        ImGui::EndChild();
        ImGui::PopID();
    }

    ImGui::Spacing();

    if (button("CREATE NEW WALLET"))
        openScreen(Screen::CreateWallet);
}

void drawCreateWalletScreen() {
    drawHeader("Create wallet");

    if (!gWalletStore) {
        ImGui::TextWrapped("Wallet storage could not be opened. No wallet can be saved.");
        return;
    }

    sectionLabel("BEFORE YOU START");
    bulletText("A new private key is generated on this device and sealed by the "
               "TEE-backed Keystore.");
    bulletText("You will see 24 recovery words once. They are never saved.");
    bulletText("Have pen and paper ready. Anyone with the words controls the funds.");

    ImGui::Spacing();
    ImGui::Spacing();

    if (button("CREATE WALLET", ButtonStyle::Primary))
        createStoredWallet();
}

void drawRecoveryWords() {
    const auto flags = ImGuiTableFlags_BordersOuter | ImGuiTableFlags_RowBg;
    if (!ImGui::BeginTable("recovery words", 2, flags))
        return;

    for (size_t i = 0; i < 12; ++i) {
        ImGui::TableNextRow();

        for (size_t column = 0; column < 2; ++column) {
            const size_t word = i + column * 12;

            ImGui::TableSetColumnIndex(static_cast<int>(column));
            ImGui::TextDisabled("%2zu", word + 1);
            ImGui::SameLine();
            ImGui::TextUnformatted(gRecoveryWords[word].data());
        }
    }

    ImGui::EndTable();
}

void drawBackupScreen() {
    drawHeader("Recovery phrase", false);

    if (!gPendingWallet) {
        ImGui::TextWrapped("No wallet is waiting for a backup.");
        return;
    }

    ImGui::TextDisabled("Wallet address");
    ImGui::TextWrapped("%s", gPendingWallet->name.c_str());
    ImGui::Spacing();

    drawRecoveryWords();
    ImGui::Spacing();

    if (gBackupStage == BackupStage::FirstWarning) {
        warningText("STEP 1 OF 2: Write all 24 words in order and keep them private. "
                    "They are not saved and may be lost if the app closes. "
                    "Anyone with them can take the funds in this wallet.");

        if (button("I WROTE DOWN ALL 24 WORDS", ButtonStyle::Primary))
            gBackupStage = BackupStage::FinalWarning;

        return;
    }

    warningText("STEP 2 OF 2: These words disappear after confirmation. "
                "Check your written copy now; they cannot be shown again.");

    ImGui::Checkbox("I checked every word in order", &gBackupReviewed);
    ImGui::BeginDisabled(!gBackupReviewed);

    if (button("BACKUP VERIFIED - HIDE WORDS", ButtonStyle::Primary))
        confirmBackup();

    ImGui::EndDisabled();
}

//-----------------------------------------------------------------------------
// [SECTION] Transactions
//-----------------------------------------------------------------------------

void clearTransaction() {
    gScanResult.reset();
    gSignature.reset();
    gReviewConfirmed = false;
}

void startScan() {
    clearTransaction();

    if (!gCamera) {
        gMessage = "Camera manager unavailable.";
        return;
    }

    if (cameraPermission(true)) {
        openCamera();
        return;
    }

    gCameraPermissionPending = true;
    gMessage = "Camera permission required. Allow access in the prompt or app settings.";
}

void signScannedTransaction() {
    if (!gWalletStore || !gScanResult || !gActiveWallet)
        return;

    // The private key lives only inside this scope; ~Wallet wipes it.
    auto wallet = gWalletStore->load(gActiveWallet->name);
    if (!wallet || wallet->address() != gActiveWallet->address) {
        gMessage = "Could not unlock the active wallet. Nothing was signed.";
        return;
    }

    Wallet::Signature signature;
    if (!wallet->sign(gScanResult->hash, signature)) {
        gMessage = "Signing failed. Nothing was signed.";
        return;
    }

    gSignature = signature;
    openScreen(Screen::Signature);
}

void drawScanScreen() {
    drawHeader("New transaction");

    if (!gActiveWallet) {
        ImGui::TextWrapped("Select an active wallet before scanning a transaction.");
        return;
    }

    if (collectScanResult()) {
        openScreen(Screen::Review);
        return;
    }

    if (!cameraActive()) {
        if (button("START CAMERA", ButtonStyle::Primary))
            startScan();

        return;
    }

    if (gCameraPermissionPending && button("ASK FOR CAMERA PERMISSION AGAIN"))
        startScan();

    hintText("Point the camera at the transaction QR code from your online device.");
    ImGui::Spacing();

    drawCameraPreview();
}

void drawCallDetails(const Transaction::Details& details) {
    sectionLabel("CALL");

    if (details.data.empty()) {
        ImGui::TextWrapped("None. This is a plain value transfer.");
        return;
    }

    if (!details.to) {
        ImGui::TextWrapped("Contract deployment with %zu bytes of code.", details.data.size());
        return;
    }

    std::optional<std::string> decoded;
    if (gCallDefinitions)
        decoded = gCallDefinitions->describe(details.data);

    if (!decoded) {
        const auto selector = hexString(details.data.data(), std::min<size_t>(4, details.data.size()));
        const auto text = "Unknown call " + selector + ". Add it in Known calls to "
                          "see its arguments before signing.";

        warningText(text.c_str());
        return;
    }

    // describe() yields "Function (selector match): sig" then "name: value" lines.
    if (beginDetails("call")) {
        for (const auto line : splitLines(*decoded)) {
            const size_t colon = line.find(": ");
            if (colon == std::string_view::npos)
                continue;

            const auto label = line.substr(0, colon);
            detailRow(label.starts_with("Function") ? "Function" : label, line.substr(colon + 2));
        }

        ImGui::EndTable();
    }

    hintText("A selector match does not prove what the contract actually does.");
}

void drawRawDetails(const Transaction& transaction) {
    const auto& details = transaction.details();
    sectionLabel("RAW");

    if (beginDetails("raw")) {
        detailRow("Signing hash", hexString(transaction.signingHash()));
        detailRow("Data size", std::to_string(details.data.size()) + " bytes");
        detailRow("Access list", std::to_string(details.accessList.size()) + " entries");

        ImGui::EndTable();
    }

    if (!details.data.empty() && ImGui::TreeNode("Show call data")) {
        ImGui::TextWrapped("%s", hexString(details.data.data(), details.data.size()).c_str());
        ImGui::TreePop();
    }

    if (!details.accessList.empty() && ImGui::TreeNode("Show access list")) {
        for (const auto& entry : details.accessList) {
            ImGui::TextWrapped("%s", hexString(entry.address).c_str());

            for (const auto& key : entry.storageKeys)
                hintText(hexString(key).c_str());
        }

        ImGui::TreePop();
    }

    if (details.signature)
        warningText("This QR already carries a signature. Signing creates a new one.");
}

void drawTransactionDetails(const Transaction& transaction) {
    const auto& details = transaction.details();
    const Chain* chain = findChain(details.chainId);
    const std::string chainId = std::to_string(details.chainId);
    const std::string symbol = chain ? chain->symbol : "(native units)";

    sectionLabel("TRANSACTION");

    if (beginDetails("transaction")) {
        detailRow("Network", chain ? std::string(chain->name) + " (" + chainId + ")" :
                                     "Unknown chain " + chainId);

        detailRow("To", details.to ? hexString(*details.to) : "New contract");
        detailRow("Value", formatUnits(details.value, 18) + " " + symbol);
        detailRow("Nonce", std::to_string(details.nonce));
        detailRow("Gas limit", std::to_string(details.gasLimit));
        detailRow("Max fee", formatUnits(details.maxFeePerGas, 9) + " gwei");
        detailRow("Priority fee", formatUnits(details.maxPriorityFeePerGas, 9) + " gwei");

        ImGui::EndTable();
    }

    if (!chain)
        warningText("This chain ID is not recognized. Make sure it is the network you expect.");

    drawCallDetails(details);
    drawRawDetails(transaction);
}

void drawBlindHash(const eth::Hash& hash) {
    sectionLabel("HASH");
    ImGui::TextWrapped("%s", hexString(hash).c_str());
    ImGui::Spacing();

    warningText("Only a 32-byte hash was scanned, so the transaction behind it cannot be "
                "shown. Signing it blindly can authorize anything. Continue only if you "
                "trust the device that produced it.");
}

void drawReviewScreen() {
    drawHeader("Review transaction");

    if (!gScanResult || !gActiveWallet) {
        ImGui::TextWrapped("Nothing to review.");
        return;
    }

    sectionLabel("SIGNING WALLET");
    ImGui::TextWrapped("%s", gActiveWallet->name.c_str());

    const bool blind = !gScanResult->transaction;
    if (blind)
        drawBlindHash(gScanResult->hash);
    else
        drawTransactionDetails(*gScanResult->transaction);

    ImGui::Spacing();
    ImGui::Spacing();

    ImGui::Checkbox(blind ? "I trust where this hash came from" : "I checked every detail above",
                    &gReviewConfirmed);

    ImGui::BeginDisabled(!gReviewConfirmed);

    if (button(blind ? "SIGN HASH" : "SIGN TRANSACTION", ButtonStyle::Primary))
        signScannedTransaction();

    ImGui::EndDisabled();

    if (button("REJECT", ButtonStyle::Danger))
        openScreen(Screen::Home, "Transaction rejected. Nothing was signed.");
}

void drawSignatureScreen() {
    drawHeader("Signature");

    if (!gSignature || !gScanResult || !gActiveWallet) {
        ImGui::TextWrapped("No signature to show.");
        return;
    }

    const auto& signature = *gSignature;
    const auto yParity = static_cast<uint8_t>(signature.recoveryId);

    std::array<uint8_t, 65> packed{};
    std::copy(signature.r.begin(), signature.r.end(), packed.begin());
    std::copy(signature.s.begin(), signature.s.end(), packed.begin() + 32);
    packed[64] = yParity;

    sectionLabel("SIGNED BY");
    ImGui::TextWrapped("%s", gActiveWallet->name.c_str());

    sectionLabel("SIGNATURE (r, s, y parity)");
    ImGui::TextWrapped("%s", hexString(packed).c_str());

    sectionLabel("COMPONENTS");

    if (beginDetails("signature")) {
        detailRow("r", hexString(signature.r));
        detailRow("s", hexString(signature.s));
        detailRow("y parity", std::to_string(yParity));
        detailRow("Hash", hexString(gScanResult->hash));

        ImGui::EndTable();
    }

    ImGui::Spacing();
    hintText("Enter this signature on your online device to broadcast the transaction.");
    ImGui::Spacing();

    if (button("DONE", ButtonStyle::Primary))
        openScreen(Screen::Home);
}

//-----------------------------------------------------------------------------
// [SECTION] Known calls
//-----------------------------------------------------------------------------

void loadCallDefinitions() {
    gCallDefinitions.reset();
    gCallText.clear();

    std::string error;
    auto packaged = call_storage::packaged(gApp->activity->assetManager, error);

    if (packaged) {
        auto parsed = CallDefinitions::parse(*packaged, error);
        if (parsed) {
            gCallDefinitions = std::move(parsed);
            gCallText = std::move(*packaged);
        }
    }

    if (!error.empty())
        gMessage = "Default calls: " + error;

    auto saved = call_storage::overrideText(gApp->activity->internalDataPath, error);
    if (!error.empty())
        gMessage = "Saved calls: " + error;

    if (!saved)
        return;

    auto parsed = CallDefinitions::parse(*saved, error);
    if (!parsed) {
        gMessage = "Saved calls: " + error;
        if (gCallDefinitions)
            gMessage += ". Default calls remain active.";

        return;
    }

    gCallDefinitions = std::move(parsed);
    gCallText = std::move(*saved);
}

// Validates, persists and activates a complete known calls text.
bool saveCallText(std::string text) {
    std::string error;
    auto parsed = CallDefinitions::parse(text, error);

    if (!parsed) {
        gMessage = error;
        return false;
    }

    if (!call_storage::save(gApp->activity->internalDataPath, text, error)) {
        gMessage = error;
        return false;
    }

    gCallDefinitions = std::move(parsed);
    gCallText = std::move(text);

    return true;
}

std::vector<KnownCall> knownCalls() {
    std::vector<KnownCall> calls;
    const auto lines = splitLines(gCallText);

    for (size_t i = 0; i < lines.size(); ++i) {
        const auto text = trim(lines[i].substr(0, lines[i].find('#')));
        if (!text.empty())
            calls.push_back({i, text});
    }

    return calls;
}

void addKnownCalls() {
    const auto declarations = splitDeclarations(gCallInput);
    if (declarations.empty()) {
        gMessage = "Type at least one function declaration.";
        return;
    }

    std::string text = gCallText;
    if (!text.empty() && text.back() != '\n')
        text.push_back('\n');

    for (const auto& declaration : declarations) {
        std::string error;
        if (!CallDefinitions::parse(declaration, error)) {
            if (error.starts_with("line 1: "))
                error.erase(0, 8);

            gMessage = "\"" + declaration + "\": " + error;
            return;
        }

        std::string candidate = text + declaration + "\n";
        if (!CallDefinitions::parse(candidate, error)) {
            const bool duplicate = error.find("duplicate") != std::string::npos;
            gMessage = "\"" + declaration + "\": " + (duplicate ? "already a known call" : error);
            return;
        }

        text = std::move(candidate);
    }

    if (!saveCallText(std::move(text)))
        return;

    gCallInput.clear();
    gMessage = "Added " + std::to_string(declarations.size()) +
               (declarations.size() == 1 ? " call." : " calls.");
}

void removeKnownCall(size_t line) {
    const auto lines = splitLines(gCallText);
    std::string text;

    for (size_t i = 0; i < lines.size(); ++i) {
        if (i == line)
            continue;

        text.append(lines[i]);
        text.push_back('\n');
    }

    if (saveCallText(std::move(text)))
        gMessage = "Call removed.";
}

void restoreKnownCalls() {
    std::string error;
    auto packaged = call_storage::packaged(gApp->activity->assetManager, error);
    if (!packaged) {
        gMessage = error;
        return;
    }

    auto parsed = CallDefinitions::parse(*packaged, error);
    if (!parsed || !call_storage::reset(gApp->activity->internalDataPath, error)) {
        gMessage = error;
        return;
    }

    gCallDefinitions = std::move(parsed);
    gCallText = std::move(*packaged);
    gMessage = "Default calls restored.";
}

void drawKnownCallsScreen() {
    drawHeader("Known calls");
    hintText("Transactions calling these functions have their arguments decoded before "
             "signing. Values are shown in base units.");

    sectionLabel("ADD CALLS");
    const float inputHeight = ImGui::GetTextLineHeight() * 4 + ImGui::GetStyle().FramePadding.y * 2;
    ImGui::InputTextMultiline("##new calls", &gCallInput, ImVec2(-1, inputHeight),
                              ImGuiInputTextFlags_WordWrap);

    hintText("One or more declarations, e.g. function transfer(address to, uint256 value) "
             "function approve(address spender, uint256 amount). Types: address, uint, "
             "uint8..uint256 and their [] arrays.");

    ImGui::BeginDisabled(trim(gCallInput).empty());

    if (button("ADD", ButtonStyle::Primary))
        addKnownCalls();

    ImGui::EndDisabled();

    const auto calls = knownCalls();
    const std::string title = "KNOWN CALLS (" + std::to_string(calls.size()) + ")";
    sectionLabel(title.c_str());

    if (calls.empty())
        ImGui::TextWrapped("No known calls. Every call will show as unknown.");

    std::optional<size_t> removed;

    for (size_t i = 0; i < calls.size(); ++i) {
        const auto flags = ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY;
        ImGui::PushID(static_cast<int>(i));

        if (ImGui::BeginChild("call", ImVec2(0, 0), flags)) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(calls[i].text.data(), calls[i].text.data() + calls[i].text.size());
            ImGui::PopTextWrapPos();

            if (confirmButton("REMOVE", ButtonStyle::Normal, kRowButtonHeight))
                removed = calls[i].line;
        }

        ImGui::EndChild();
        ImGui::PopID();
    }

    // Removal rewrites gCallText, which the KnownCall views point into.
    if (removed)
        removeKnownCall(*removed);

    ImGui::Spacing();

    if (confirmButton("RESTORE DEFAULT CALLS"))
        restoreKnownCalls();
}

//-----------------------------------------------------------------------------
// [SECTION] Main window
//-----------------------------------------------------------------------------

void drawHomeScreen() {
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.6f);
    ImGui::TextUnformatted("Airgap");
    ImGui::PopFont();

    hintText("Offline Ethereum signer");
    ImGui::Spacing();
    drawMessage();

    const auto flags = ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY;

    if (ImGui::BeginChild("active wallet", ImVec2(0, 0), flags)) {
        ImGui::TextDisabled("ACTIVE WALLET");

        if (gActiveWallet)
            ImGui::TextWrapped("%s", gActiveWallet->name.c_str());
        else
            hintText("None selected. Choose one in Wallets or create a new wallet.");
    }

    ImGui::EndChild();
    ImGui::Spacing();

    ImGui::BeginDisabled(!gActiveWallet);

    if (button("NEW TRANSACTION", ButtonStyle::Primary))
        openScreen(Screen::Scan);

    ImGui::EndDisabled();

    const size_t walletCount = gWalletStore ? gWalletStore->list().size() : 0;
    const size_t callCount = gCallDefinitions ? gCallDefinitions->size() : 0;
    const std::string wallets = "WALLETS (" + std::to_string(walletCount) + ")";
    const std::string calls = "KNOWN CALLS (" + std::to_string(callCount) + ")";

    if (button(wallets.c_str()))
        openScreen(Screen::Wallets);

    if (button("CREATE WALLET"))
        openScreen(Screen::CreateWallet);

    if (button(calls.c_str()))
        openScreen(Screen::KnownCalls);
}

void drawScreen() {
    switch (gScreen) {
    case Screen::Home:         drawHomeScreen(); break;
    case Screen::KnownCalls:   drawKnownCallsScreen(); break;

    case Screen::Wallets:      drawWalletsScreen(); break;
    case Screen::CreateWallet: drawCreateWalletScreen(); break;
    case Screen::Backup:       drawBackupScreen(); break;

    case Screen::Scan:         drawScanScreen(); break;
    case Screen::Review:       drawReviewScreen(); break;
    case Screen::Signature:    drawSignatureScreen(); break;
    }
}

// Leaving a screen releases what it owns: camera, transaction, recovery words.
void applyNavigation() {
    if (!gNextScreen)
        return;

    const Screen screen = *std::exchange(gNextScreen, std::nullopt);
    const bool transactionFlow = screen == Screen::Scan || screen == Screen::Review ||
                                 screen == Screen::Signature;

    if (screen != Screen::Scan)
        closeCamera();

    if (!transactionFlow)
        clearTransaction();

    if (screen != Screen::Backup)
        clearRecoveryWords();

    gScreen = screen;
    gMessage = std::move(gNextMessage);
    gNextMessage.clear();
    gArmedButton = 0;

    // Show the new screen from the top, even while a finger is still dragging.
    ImGui::SetScrollY(0.0f);
    gScrollVelocity = 0.0f;
    gScrollStartY = gLastDragY;

    if (screen == Screen::Scan)
        startScan();
}

// Keeps the page gliding after a fling, slowing until it stops or reaches an edge.
void glideScroll(float deltaTime) {
    if (gScrollVelocity == 0.0f)
        return;

    const float scroll = ImGui::GetScrollY() + gScrollVelocity * deltaTime;
    ImGui::SetScrollY(scroll);
    gScrollVelocity *= std::exp(-kGlideFriction * deltaTime);

    const bool atEdge = scroll <= 0.0f || scroll >= ImGui::GetScrollMaxY();
    if (atEdge || std::abs(gScrollVelocity) < kMinGlide)
        gScrollVelocity = 0.0f;
}

// Touch screens have no scroll wheel: a drag anywhere scrolls the main window.
// Once a touch scrolls, the widget under the finger is cancelled so releasing
// does not click it. A touch on a gliding page only stops the glide.
void updateTouchScroll() {
    const ImGuiIO& io = ImGui::GetIO();
    const float deltaTime = std::max(io.DeltaTime, 0.001f);

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        gTouchScrolling = gScrollVelocity != 0.0f;
        gScrollVelocity = 0.0f;
        gScrollStartY = ImGui::GetScrollY();
        gLastDragY = 0.0f;
    }

    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        // A quick tap that stopped a glide was activated and released in two frames.
        if (gTouchScrolling)
            ImGui::ClearActiveID();

        gTouchScrolling = false;
        glideScroll(deltaTime);
        return;
    }

    const float slop = io.MouseDragThreshold * kUiScale;
    gTouchScrolling |= ImGui::IsMouseDragging(ImGuiMouseButton_Left, slop);

    if (!gTouchScrolling)
        return;

    ImGui::ClearActiveID();

    // Follow the finger; rebasing at the edges avoids a dead zone when reversing.
    const float dragY = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f).y;
    const float moved = dragY - std::exchange(gLastDragY, dragY);
    const float scroll = std::clamp(gScrollStartY - dragY, 0.0f, ImGui::GetScrollMaxY());

    gScrollStartY = scroll + dragY;
    ImGui::SetScrollY(scroll);

    // Smooth the finger speed so a fling keeps its momentum after release.
    gScrollVelocity += (-moved / deltaTime - gScrollVelocity) * 0.4f;
}

//-----------------------------------------------------------------------------
// [SECTION] Graphics lifecycle
//-----------------------------------------------------------------------------

void applyStyle() {
    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowBorderSize = 0.0f;
    style.FramePadding = ImVec2(8.0f, 6.0f);
    style.ItemSpacing = ImVec2(8.0f, 8.0f);
    style.FrameRounding = 6.0f;
    style.ChildRounding = 8.0f;
    style.GrabRounding = 6.0f;

    style.Colors[ImGuiCol_Button] = ImVec4(0.20f, 0.22f, 0.27f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.29f, 0.35f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.16f, 0.18f, 0.22f, 1.0f);

    // Scale UI for a phone screen.
    style.ScaleAllSizes(kUiScale);
    style.FontScaleDpi = kUiScale;
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

    // Dear ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    // We don't need imgui.ini for a fixed wallet UI.
    ImGui::GetIO().IniFilename = nullptr;

    applyStyle();
    ImGui_ImplAndroid_Init(app->window);
    ImGui_ImplOpenGL3_Init("#version 300 es");

    gInitialized = true;
    LOGI("ImGui initialized");

    return true;
}

void shutdownGraphics() {
    closeCamera();
    if (!gInitialized)
        return;

    if (gKeyboardRequested)
        ANativeActivity_hideSoftInput(gApp->activity, 0);

    gKeyboardRequested = false;

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

//-----------------------------------------------------------------------------
// [SECTION] Public API
//-----------------------------------------------------------------------------

void initialize(android_app* app) {
    gApp = app;
    ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_SECURE, 0);
    gWalletStore = WalletStore::open(app->activity);
    gActiveWallet.reset();
    clearRecoveryWords();
    clearTransaction();

    gScreen = Screen::Home;
    gNextScreen.reset();
    gMessage.clear();
    gArmedButton = 0;
    gKeyboardRequested = false;

    loadCallDefinitions();
    gCamera = Camera::init();
}

void shutdown() {
    closeCamera();
    clearTransaction();
    shutdownGraphics();
    clearRecoveryWords();
    gActiveWallet.reset();
    gWalletStore.reset();

    gCallDefinitions.reset();
    gCallText.clear();
    gCallInput.clear();
    gCamera.reset();
}

bool isReady() {
    return gInitialized;
}

void drawFrame() {
    if (!gInitialized)
        return;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGui::NewFrame();

    // Full-width window below the status bar.
    const ImGuiIO& io = ImGui::GetIO();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoScrollbar;

    ImGui::SetNextWindowPos(ImVec2(0, io.DisplaySize.y * 0.1f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, io.DisplaySize.y * 0.9f), ImGuiCond_Always);
    ImGui::Begin("Airgap", nullptr, flags);

    applyNavigation();
    updateTouchScroll();
    drawScreen();

    ImGui::End();
    ImGui::Render();
    updateSoftKeyboard();

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

    case APP_CMD_PAUSE: {
        gResumed = false;

        // Preserve a pending permission request while its dialog is visible.
        const bool pending = gCameraPermissionPending;
        closeCamera();
        gCameraPermissionPending = pending;
        break;
    }

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
    const int32_t handled = ImGui_ImplAndroid_HandleInputEvent(event);
    addTextInput(event);

    return handled;
}

} // namespace ui
