#include "wallet_store.hpp"

#include <android/api-level.h>
#include <android/native_activity.h>
#include <jni.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdarg>
#include <fcntl.h>
#include <map>
#include <mutex>

#include <span>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {

constexpr uint8_t kMagic = 0xaa;
constexpr uint8_t kLegacyVersion = 1;
constexpr uint8_t kVersion = 2;
constexpr size_t kHeaderSize = 10;
constexpr size_t kMaxKeys = 128;
constexpr size_t kMaxName = 64;
constexpr size_t kMaxFile = 32768;

// v1 keys need no user authentication; they are only read to re-seal wallets.
constexpr char kLegacyKeyAlias[] = "airgap.wallet-store.v1";
constexpr char kKeyAlias[] = "airgap.wallet-store.v2";
constexpr char kPromptClass[] = "com.paoloanzn.airgap.AuthPrompt";

constexpr int kEncryptMode = 1;
constexpr int kDecryptMode = 2;
constexpr int kTeeSecurityLevel = 1;
constexpr int kAuthDeviceCredential = 1;
constexpr int kAuthBiometricStrong = 2;

class JniScope {
public:
    explicit JniScope(ANativeActivity* activity) {
        if (!activity || !activity->vm)
            return;

        vm_ = activity->vm;
        const jint state = vm_->GetEnv(reinterpret_cast<void**>(&env_),
                                      JNI_VERSION_1_6);

        if (state == JNI_EDETACHED) {
            attached_ = vm_->AttachCurrentThread(&env_, nullptr) == JNI_OK;
            if (!attached_)
                return;
        }

        if (state != JNI_OK && state != JNI_EDETACHED)
            return;

        ready_ = env_->PushLocalFrame(128) == JNI_OK;
        frame_ = ready_;
    }

    ~JniScope() {
        if (env_ && env_->ExceptionCheck())
            env_->ExceptionClear();

        if (frame_)
            env_->PopLocalFrame(nullptr);

        if (attached_)
            vm_->DetachCurrentThread();
    }

    bool ready() const { return ready_; }
    JNIEnv* env() const { return env_; }

    bool check() {
        if (!ready_ || !env_->ExceptionCheck())
            return ready_;

        env_->ExceptionClear();
        ready_ = false;
        return false;
    }

    jclass find(const char* name) {
        if (!ready_)
            return nullptr;

        jclass result = env_->FindClass(name);
        return check() ? result : nullptr;
    }

    jstring string(const char* text) {
        if (!ready_)
            return nullptr;

        jstring result = env_->NewStringUTF(text);
        return check() ? result : nullptr;
    }

    jobject make(const char* className, const char* signature, ...) {
        jclass type = find(className);
        if (!type)
            return nullptr;

        jmethodID method = env_->GetMethodID(type, "<init>", signature);
        if (!method || !check())
            return nullptr;

        va_list args;
        va_start(args, signature);
        jobject result = env_->NewObjectV(type, method, args);
        va_end(args);
        return check() ? result : nullptr;
    }

    jobject staticObject(const char* className, const char* name,
                         const char* signature, ...) {
        jclass type = find(className);
        if (!type)
            return nullptr;

        jmethodID method = env_->GetStaticMethodID(type, name, signature);
        if (!method || !check())
            return nullptr;

        va_list args;
        va_start(args, signature);
        jobject result = env_->CallStaticObjectMethodV(type, method, args);
        va_end(args);
        return check() ? result : nullptr;
    }

    jobject object(jobject target, const char* name,
                   const char* signature, ...) {
        if (!ready_ || !target)
            return nullptr;

        jclass type = env_->GetObjectClass(target);
        if (!type || !check())
            return nullptr;

        jmethodID method = env_->GetMethodID(type, name, signature);
        if (!method || !check())
            return nullptr;

        va_list args;
        va_start(args, signature);
        jobject result = env_->CallObjectMethodV(target, method, args);
        va_end(args);
        return check() ? result : nullptr;
    }

    bool call(jobject target, const char* name,
              const char* signature, ...) {
        if (!ready_ || !target)
            return false;

        jclass type = env_->GetObjectClass(target);
        if (!type || !check())
            return false;

        jmethodID method = env_->GetMethodID(type, name, signature);
        if (!method || !check())
            return false;

        va_list args;
        va_start(args, signature);
        env_->CallVoidMethodV(target, method, args);
        va_end(args);
        return check();
    }

    int integer(jobject target, const char* name,
                const char* signature) {
        if (!ready_ || !target)
            return -1;

        jclass type = env_->GetObjectClass(target);
        if (!type || !check())
            return -1;

        jmethodID method = env_->GetMethodID(type, name, signature);
        if (!method || !check())
            return -1;

        const jint result = env_->CallIntMethod(target, method);
        return check() ? result : -1;
    }

    bool flag(jobject target, const char* name, const char* signature) {
        if (!ready_ || !target)
            return false;

        jclass type = env_->GetObjectClass(target);
        if (!type || !check())
            return false;

        jmethodID method = env_->GetMethodID(type, name, signature);
        if (!method || !check())
            return false;

        const jboolean result = env_->CallBooleanMethod(target, method);
        return check() && result == JNI_TRUE;
    }

    jbyteArray bytes(const uint8_t* data, size_t size) {
        if (!ready_ || size > static_cast<size_t>(INT32_MAX))
            return nullptr;

        jbyteArray result = env_->NewByteArray(static_cast<jsize>(size));
        if (!result || !check())
            return nullptr;

        env_->SetByteArrayRegion(result, 0, static_cast<jsize>(size),
                                 reinterpret_cast<const jbyte*>(data));
        return check() ? result : nullptr;
    }

    bool copy(jbyteArray source, uint8_t* target, size_t size) {
        if (!ready_ || !source || !target)
            return false;

        if (env_->GetArrayLength(source) != static_cast<jsize>(size))
            return false;

        env_->GetByteArrayRegion(source, 0, static_cast<jsize>(size),
                                 reinterpret_cast<jbyte*>(target));
        return check();
    }

    void clear(jbyteArray bytes, size_t size) {
        if (!bytes || !env_ || size > 64)
            return;

        std::array<jbyte, 64> zeros{};
        env_->SetByteArrayRegion(bytes, 0, static_cast<jsize>(size),
                                 zeros.data());
        check();
    }

private:
    JavaVM* vm_ = nullptr;
    JNIEnv* env_ = nullptr;
    bool attached_ = false;
    bool frame_ = false;
    bool ready_ = false;
};

// Keystore keys ---------------------------------------------------------------

// The key version is authenticated too, so a v2 entry cannot pass as v1.
std::vector<uint8_t> associatedData(const StoredKey& entry) {
    std::vector<uint8_t> data{kMagic, entry.keyVersion,
                              static_cast<uint8_t>(entry.name.size())};

    data.insert(data.end(), entry.name.begin(), entry.name.end());
    data.insert(data.end(), entry.address.begin(), entry.address.end());
    return data;
}

// Every use of this key needs its own biometric or device credential
// authentication (timeout 0). Enrolling a new fingerprint keeps it valid;
// removing the screen lock invalidates it permanently.
jobject generateKeystoreKey(JniScope& jni) {
    auto alias = jni.string(kKeyAlias);
    auto algorithm = jni.string("AES");
    auto provider = jni.string("AndroidKeyStore");

    auto gcm = jni.string("GCM");
    auto noPadding = jni.string("NoPadding");
    auto stringClass = jni.find("java/lang/String");

    if (!alias || !algorithm || !provider || !stringClass ||
        !gcm || !noPadding)
        return nullptr;

    auto* env = jni.env();
    auto modes = env->NewObjectArray(1, stringClass, gcm);
    auto paddings = env->NewObjectArray(1, stringClass, noPadding);
    if (!modes || !paddings || !jni.check())
        return nullptr;

    auto builder = jni.make(
        "android/security/keystore/KeyGenParameterSpec$Builder",
        "(Ljava/lang/String;I)V", alias,
        kEncryptMode | kDecryptMode);
    if (!builder)
        return nullptr;

    if (!jni.object(builder, "setBlockModes",
                    "([Ljava/lang/String;)" "Landroid/security/keystore/KeyGenParameterSpec$Builder;",
                    modes))
        return nullptr;

    if (!jni.object(builder, "setEncryptionPaddings",
                    "([Ljava/lang/String;)" "Landroid/security/keystore/KeyGenParameterSpec$Builder;",
                    paddings))
        return nullptr;

    if (!jni.object(builder, "setKeySize", "(I)Landroid/security/keystore/KeyGenParameterSpec$Builder;",
                    256))
        return nullptr;

    if (!jni.object(builder, "setUserAuthenticationRequired",
                    "(Z)Landroid/security/keystore/KeyGenParameterSpec$Builder;", JNI_TRUE))
        return nullptr;

    if (!jni.object(builder, "setUserAuthenticationParameters",
                    "(II)Landroid/security/keystore/KeyGenParameterSpec$Builder;",
                    0, kAuthBiometricStrong | kAuthDeviceCredential))
        return nullptr;

    if (!jni.object(builder, "setInvalidatedByBiometricEnrollment",
                    "(Z)Landroid/security/keystore/KeyGenParameterSpec$Builder;", JNI_FALSE))
        return nullptr;

    auto spec = jni.object(builder, "build",
                           "()Landroid/security/keystore/KeyGenParameterSpec;");
    auto generator = jni.staticObject(
        "javax/crypto/KeyGenerator", "getInstance",
        "(Ljava/lang/String;Ljava/lang/String;)Ljavax/crypto/KeyGenerator;",
        algorithm, provider);

    if (!spec || !generator)
        return nullptr;

    if (!jni.call(generator, "init",
                  "(Ljava/security/spec/AlgorithmParameterSpec;)V", spec))
        return nullptr;

    return jni.object(generator, "generateKey",
                      "()Ljavax/crypto/SecretKey;");
}

bool isTeeKey(JniScope& jni, jobject key) {
    auto algorithm = jni.string("AES");
    auto provider = jni.string("AndroidKeyStore");
    auto infoClass = jni.find("android/security/keystore/KeyInfo");
    if (!algorithm || !provider || !infoClass)
        return false;

    auto factory = jni.staticObject(
        "javax/crypto/SecretKeyFactory", "getInstance",
        "(Ljava/lang/String;Ljava/lang/String;)Ljavax/crypto/SecretKeyFactory;",
        algorithm, provider);
    if (!factory)
        return false;

    auto info = jni.object(
        factory, "getKeySpec",
        "(Ljavax/crypto/SecretKey;Ljava/lang/Class;)Ljava/security/spec/KeySpec;",
        key, infoClass);
    if (!info)
        return false;

    return jni.integer(info, "getSecurityLevel", "()I") ==
           kTeeSecurityLevel;
}

jobject keyStore(JniScope& jni) {
    auto provider = jni.string("AndroidKeyStore");
    if (!provider)
        return nullptr;

    auto store = jni.staticObject(
        "java/security/KeyStore", "getInstance",
        "(Ljava/lang/String;)Ljava/security/KeyStore;", provider);
    if (!store || !jni.call(store, "load",
                            "(Ljava/io/InputStream;[C)V", nullptr, nullptr))
        return nullptr;

    return store;
}

// Only the v2 key is ever created; the legacy key is looked up, never made.
jobject secretKey(JniScope& jni, const char* aliasName, bool create) {
    if (android_get_device_api_level() < 31)
        return nullptr;

    auto store = keyStore(jni);
    auto alias = jni.string(aliasName);
    if (!store || !alias)
        return nullptr;

    auto key = jni.object(store, "getKey",
                          "(Ljava/lang/String;[C)Ljava/security/Key;",
                          alias, nullptr);
    if (!jni.ready())
        return nullptr;

    if (key)
        return isTeeKey(jni, key) ? key : nullptr;

    if (!create || std::string_view(aliasName) != kKeyAlias)
        return nullptr;

    key = generateKeystoreKey(jni);
    if (!key)
        return nullptr;

    if (isTeeKey(jni, key))
        return key;

    jni.call(store, "deleteEntry", "(Ljava/lang/String;)V", alias);
    return nullptr;
}

void deleteKey(JniScope& jni, const char* aliasName) {
    auto store = keyStore(jni);
    auto alias = jni.string(aliasName);

    if (store && alias)
        jni.call(store, "deleteEntry", "(Ljava/lang/String;)V", alias);
}

bool deviceSecure(JniScope& jni, ANativeActivity* activity) {
    auto service = jni.string("keyguard");
    auto keyguard = jni.object(activity->clazz, "getSystemService",
                               "(Ljava/lang/String;)Ljava/lang/Object;", service);

    return jni.flag(keyguard, "isDeviceSecure", "()Z");
}

// Decryption passes the stored nonce; encryption lets Keystore pick a fresh one.
jobject initCipher(JniScope& jni, jobject secret, int mode, const uint8_t* iv) {
    auto transformation = jni.string("AES/GCM/NoPadding");
    if (!secret || !transformation)
        return nullptr;

    auto cipher = jni.staticObject(
        "javax/crypto/Cipher", "getInstance",
        "(Ljava/lang/String;)Ljavax/crypto/Cipher;", transformation);
    if (!cipher)
        return nullptr;

    if (!iv)
        return jni.call(cipher, "init", "(ILjava/security/Key;)V", mode, secret) ? cipher : nullptr;

    auto javaIv = jni.bytes(iv, 12);
    auto params = javaIv ? jni.make("javax/crypto/spec/GCMParameterSpec", "(I[B)V", 128, javaIv)
                         : nullptr;

    if (!params || !jni.call(
            cipher, "init",
            "(ILjava/security/Key;Ljava/security/spec/AlgorithmParameterSpec;)V",
            mode, secret, params))
        return nullptr;

    return cipher;
}

bool encryptWith(JniScope& jni, jobject cipher, const eth::PrivateKey& key,
                 StoredKey& entry) {
    auto aad = associatedData(entry);
    auto javaAad = jni.bytes(aad.data(), aad.size());
    if (!javaAad || !jni.call(cipher, "updateAAD", "([B)V", javaAad))
        return false;

    auto plaintext = jni.bytes(key.data(), key.size());
    if (!plaintext)
        return false;

    auto ciphertext = static_cast<jbyteArray>(
        jni.object(cipher, "doFinal", "([B)[B", plaintext));
    jni.clear(plaintext, key.size());
    if (!ciphertext)
        return false;

    auto iv = static_cast<jbyteArray>(jni.object(cipher, "getIV", "()[B"));
    return jni.copy(iv, entry.encryptedKey.data(), 12) &&
           jni.copy(ciphertext, entry.encryptedKey.data() + 12, 48);
}

bool decryptWith(JniScope& jni, jobject cipher, const StoredKey& entry,
                 eth::PrivateKey& output) {
    auto aad = associatedData(entry);
    auto javaAad = jni.bytes(aad.data(), aad.size());
    if (!javaAad || !jni.call(cipher, "updateAAD", "([B)V", javaAad))
        return false;

    auto ciphertext = jni.bytes(entry.encryptedKey.data() + 12, 48);
    auto plaintext = static_cast<jbyteArray>(
        jni.object(cipher, "doFinal", "([B)[B", ciphertext));
    if (!plaintext)
        return false;

    const bool copied = jni.copy(plaintext, output.data(), output.size());
    jni.clear(plaintext, output.size());
    return copied;
}

bool decryptLegacy(JniScope& jni, const StoredKey& entry, eth::PrivateKey& output) {
    auto secret = secretKey(jni, kLegacyKeyAlias, false);
    auto cipher = initCipher(jni, secret, kDecryptMode, entry.encryptedKey.data());

    return cipher && decryptWith(jni, cipher, entry, output);
}

// System authentication prompt ------------------------------------------------

// Results arrive on the Java main thread; requests poll them by id. A request
// removes its id when destroyed, so late callbacks are ignored.
struct PromptResult {
    bool finished = false;
    bool success = false;
    std::string message;
};

std::mutex gPromptMutex;
std::map<jlong, PromptResult> gPrompts;
jlong gNextPromptId = 1;
jclass gPromptClass = nullptr;

void JNICALL onPromptResult(JNIEnv* env, jclass, jlong id, jboolean success,
                            jstring message) {
    std::string text;

    if (const char* chars = message ? env->GetStringUTFChars(message, nullptr) : nullptr) {
        text = chars;
        env->ReleaseStringUTFChars(message, chars);
    }

    const std::lock_guard lock(gPromptMutex);
    const auto found = gPrompts.find(id);

    if (found != gPrompts.end() && !found->second.finished)
        found->second = {true, success == JNI_TRUE, std::move(text)};
}

// App classes are only visible through the activity's class loader.
jclass promptClass(JniScope& jni, ANativeActivity* activity) {
    if (gPromptClass)
        return gPromptClass;

    auto name = jni.string(kPromptClass);
    auto loader = jni.object(activity->clazz, "getClassLoader", "()Ljava/lang/ClassLoader;");
    auto type = static_cast<jclass>(jni.object(
        loader, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;", name));
    if (!type)
        return nullptr;

    const JNINativeMethod method{"nativeResult", "(JZLjava/lang/String;)V",
                                 reinterpret_cast<void*>(onPromptResult)};

    auto* env = jni.env();
    if (env->RegisterNatives(type, &method, 1) != JNI_OK || !jni.check())
        return nullptr;

    gPromptClass = static_cast<jclass>(env->NewGlobalRef(type));
    return gPromptClass;
}

// Returns a global reference to the prompt's CancellationSignal.
jobject showPrompt(JniScope& jni, ANativeActivity* activity, jobject cipher,
                   const WalletStore::Prompt& prompt, jlong id) {
    auto type = promptClass(jni, activity);

    auto title = jni.string(prompt.title.c_str());
    auto subtitle = jni.string(prompt.subtitle.c_str());
    if (!type || !title || !subtitle)
        return nullptr;

    auto* env = jni.env();
    auto show = env->GetStaticMethodID(
        type, "show",
        "(Landroid/app/Activity;Ljavax/crypto/Cipher;Ljava/lang/String;Ljava/lang/String;J)"
        "Landroid/os/CancellationSignal;");
    if (!show || !jni.check())
        return nullptr;

    auto cancel = env->CallStaticObjectMethod(type, show, activity->clazz, cipher,
                                              title, subtitle, id);
    if (!cancel || !jni.check())
        return nullptr;

    return env->NewGlobalRef(cancel);
}

// File format -----------------------------------------------------------------

class FileDescriptor {
public:
    explicit FileDescriptor(int value) : value_(value) {}

    ~FileDescriptor() {
        if (value_ >= 0)
            ::close(value_);
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    int get() const { return value_; }

private:
    int value_;
};

bool validName(std::string_view name) {
    if (name.empty() || name.size() > kMaxName)
        return false;

    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return c >= 0x20 && c != 0x7f;
    });
}

void append32(std::vector<uint8_t>& bytes, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<uint8_t>(value >> shift));
}

uint32_t read32(std::span<const uint8_t> bytes, size_t offset) {
    uint32_t value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8)
        value |= uint32_t(bytes[offset++]) << shift;

    return value;
}

std::optional<std::vector<uint8_t>> encode(const Envelope& envelope) {
    if (envelope.keys.size() > kMaxKeys)
        return std::nullopt;

    size_t size = kHeaderSize;
    for (const auto& entry : envelope.keys) {
        if (!validName(entry.name))
            return std::nullopt;

        size += 2 + entry.encryptedKey.size() +
                entry.address.size() + entry.name.size();
    }

    if (size > kMaxFile)
        return std::nullopt;

    std::vector<uint8_t> bytes;
    bytes.reserve(size);
    bytes.push_back(kMagic);
    bytes.push_back(kVersion);
    append32(bytes, static_cast<uint32_t>(envelope.keys.size()));
    append32(bytes, static_cast<uint32_t>(size));

    for (const auto& entry : envelope.keys) {
        bytes.push_back(static_cast<uint8_t>(entry.name.size()));
        bytes.push_back(entry.keyVersion);
        bytes.insert(bytes.end(), entry.encryptedKey.begin(),
                     entry.encryptedKey.end());

        bytes.insert(bytes.end(), entry.address.begin(), entry.address.end());
        bytes.insert(bytes.end(), entry.name.begin(), entry.name.end());
    }

    return bytes;
}

std::optional<Envelope> decode(std::span<const uint8_t> bytes) {
    if (bytes.size() < kHeaderSize || bytes.size() > kMaxFile || bytes[0] != kMagic)
        return std::nullopt;

    const uint8_t version = bytes[1];
    if (version != kLegacyVersion && version != kVersion)
        return std::nullopt;

    const uint32_t count = read32(bytes, 2);
    const uint32_t size = read32(bytes, 6);
    if (count > kMaxKeys || size != bytes.size())
        return std::nullopt;

    // Name length, key version (v2 only), encrypted key, address, then name.
    const size_t fixedSize = version == kVersion ? 82 : 81;

    Envelope envelope;
    envelope.keys.reserve(count);
    size_t offset = kHeaderSize;

    for (uint32_t index = 0; index < count; ++index) {
        if (bytes.size() - offset < fixedSize)
            return std::nullopt;

        StoredKey entry;
        const size_t nameSize = bytes[offset++];
        entry.keyVersion = version == kVersion ? bytes[offset++] : kLegacyVersion;

        if (entry.keyVersion != kLegacyVersion && entry.keyVersion != kVersion)
            return std::nullopt;

        if (nameSize == 0 || nameSize > kMaxName ||
            bytes.size() - offset < 80 + nameSize)
            return std::nullopt;

        std::copy_n(bytes.data() + offset, 60, entry.encryptedKey.data());
        offset += 60;

        std::copy_n(bytes.data() + offset, 20, entry.address.data());
        offset += 20;
        entry.name.assign(reinterpret_cast<const char*>(bytes.data() + offset),
                          nameSize);
        offset += nameSize;

        const auto duplicate = std::find_if(
            envelope.keys.begin(), envelope.keys.end(),
            [&](const StoredKey& key) { return key.name == entry.name; });
        if (!validName(entry.name) || duplicate != envelope.keys.end())
            return std::nullopt;

        envelope.keys.push_back(std::move(entry));
    }

    return offset == bytes.size() ? std::optional<Envelope>(std::move(envelope))
                                  : std::nullopt;
}

std::optional<Envelope> readFile(const std::string& path) {
    FileDescriptor file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (file.get() < 0)
        return errno == ENOENT ? std::optional<Envelope>(Envelope{})
                               : std::nullopt;

    struct stat info{};
    if (::fstat(file.get(), &info) != 0 || !S_ISREG(info.st_mode) ||
        (info.st_mode & 077) != 0 || info.st_nlink != 1 ||
        info.st_size < 0 || info.st_size > static_cast<off_t>(kMaxFile))
        return std::nullopt;

    std::vector<uint8_t> bytes(static_cast<size_t>(info.st_size));
    size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::read(file.get(), bytes.data() + offset,
                                     bytes.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;

        if (count <= 0)
            return std::nullopt;

        offset += static_cast<size_t>(count);
    }

    return decode(bytes);
}

bool writeAll(int fd, std::span<const uint8_t> bytes) {
    size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::write(fd, bytes.data() + offset,
                                      bytes.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;

        if (count <= 0)
            return false;

        offset += static_cast<size_t>(count);
    }

    return true;
}

std::optional<std::string> temporaryPath(const std::string& path) {
    std::array<uint8_t, 8> random{};
    if (RAND_bytes(random.data(), random.size()) != 1)
        return std::nullopt;

    constexpr char digits[] = "0123456789abcdef";
    std::string result = path + ".tmp-";
    for (uint8_t byte : random) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }

    return result;
}

} // namespace

// WalletStore::Request --------------------------------------------------------

WalletStore::Request::Request(WalletStore& store, Kind kind)
    : store_(store), kind_(kind) {}

WalletStore::Request::~Request() {
    release();
}

WalletStore::Request::Status WalletStore::Request::poll() {
    if (status_ != Status::Waiting)
        return status_;

    PromptResult result;

    {
        const std::lock_guard lock(gPromptMutex);
        const auto found = gPrompts.find(promptId_);
        if (found != gPrompts.end())
            result = found->second;
    }

    if (!result.finished)
        return Status::Waiting;

    if (!result.success)
        return fail(result.message.empty() ? "Authentication failed." : result.message);

    if (!finish())
        return fail(kind_ == Kind::Save ? "The wallet could not be saved." :
                                          "The wallet could not be unlocked.");

    release();
    status_ = Status::Done;
    return status_;
}

std::optional<Wallet> WalletStore::Request::takeWallet() {
    if (status_ != Status::Done || kind_ == Kind::Save)
        return std::nullopt;

    return std::exchange(wallet_, std::nullopt);
}

WalletStore::Request::Status WalletStore::Request::fail(std::string error) {
    status_ = Status::Failed;
    error_ = std::move(error);
    wallet_.reset();
    release();

    return status_;
}

// Runs the single key operation the user just authorized.
bool WalletStore::Request::finish() {
    JniScope jni(store_.activity_);
    eth::PrivateKey key{};

    if (kind_ == Kind::Save) {
        const bool saved = !store_.find(entry_.name) &&
                           encryptWith(jni, cipher_, wallet_->privateKey_, entry_) &&
                           store_.commit(entry_);
        wallet_.reset();
        return saved;
    }

    if (kind_ == Kind::Load) {
        if (decryptWith(jni, cipher_, entry_, key))
            wallet_ = Wallet::load(std::move(key));

        OPENSSL_cleanse(key.data(), key.size());
        return wallet_ && wallet_->address() == entry_.address;
    }

    // Reseal: move a v1 wallet under the authentication-bound v2 key.
    const bool decrypted = decryptLegacy(jni, entry_, key);
    entry_.keyVersion = kVersion;

    if (decrypted && encryptWith(jni, cipher_, key, entry_) && store_.commit(entry_))
        wallet_ = Wallet::load(std::move(key));

    OPENSSL_cleanse(key.data(), key.size());

    const bool legacyLeft = std::any_of(
        store_.envelope_.keys.begin(), store_.envelope_.keys.end(),
        [](const StoredKey& stored) { return stored.keyVersion == kLegacyVersion; });

    if (wallet_ && !legacyLeft)
        deleteKey(jni, kLegacyKeyAlias);

    return wallet_ && wallet_->address() == entry_.address;
}

void WalletStore::Request::release() {
    {
        const std::lock_guard lock(gPromptMutex);
        gPrompts.erase(promptId_);
    }

    if (!cipher_ && !cancel_)
        return;

    JniScope jni(store_.activity_);
    if (!jni.ready())
        return;

    // Cancelling a finished prompt is a no-op.
    jni.call(cancel_, "cancel", "()V");

    if (cipher_)
        jni.env()->DeleteGlobalRef(cipher_);

    if (cancel_)
        jni.env()->DeleteGlobalRef(cancel_);

    cipher_ = cancel_ = nullptr;
}

// WalletStore -----------------------------------------------------------------

WalletStore::WalletStore(ANativeActivity* activity, std::string path)
    : activity_(activity), path_(std::move(path)) {}

std::optional<WalletStore> WalletStore::open(ANativeActivity* activity) {
    if (!activity || !activity->vm || !activity->internalDataPath)
        return std::nullopt;

    std::string path = std::string(activity->internalDataPath) +
                       "/wallets.bin";
    auto envelope = readFile(path);
    if (!envelope)
        return std::nullopt;

    WalletStore store(activity, std::move(path));
    store.envelope_ = std::move(*envelope);
    return store;
}

std::vector<WalletStore::WalletInfo> WalletStore::list() const {
    std::vector<WalletInfo> result;
    result.reserve(envelope_.keys.size());

    for (const auto& entry : envelope_.keys)
        result.push_back({entry.name, entry.address});

    return result;
}

const StoredKey* WalletStore::find(std::string_view name) const {
    const auto found = std::find_if(
        envelope_.keys.begin(), envelope_.keys.end(),
        [&](const StoredKey& key) { return key.name == name; });

    return found == envelope_.keys.end() ? nullptr : &*found;
}

bool WalletStore::write(const Envelope& envelope) const {
    auto bytes = encode(envelope);
    auto temp = temporaryPath(path_);
    if (!bytes || !temp)
        return false;

    FileDescriptor file(::open(temp->c_str(),
                               O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                                   O_NOFOLLOW,
                               S_IRUSR | S_IWUSR));
    if (file.get() < 0)
        return false;

    if (!writeAll(file.get(), *bytes) || ::fsync(file.get()) != 0) {
        ::unlink(temp->c_str());
        return false;
    }

    if (::rename(temp->c_str(), path_.c_str()) != 0) {
        ::unlink(temp->c_str());
        return false;
    }

    FileDescriptor directory(::open(activity_->internalDataPath,
                                    O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (directory.get() >= 0)
        ::fsync(directory.get());

    return true;
}

// Replaces the entry with the same name, or appends a new one.
bool WalletStore::commit(const StoredKey& entry) {
    Envelope next = envelope_;
    const auto existing = std::find_if(
        next.keys.begin(), next.keys.end(),
        [&](const StoredKey& key) { return key.name == entry.name; });

    if (existing != next.keys.end())
        *existing = entry;
    else if (next.keys.size() < kMaxKeys)
        next.keys.push_back(entry);
    else
        return false;

    if (!write(next))
        return false;

    envelope_ = std::move(next);
    return true;
}

std::unique_ptr<WalletStore::Request> WalletStore::save(std::string_view name,
                                                        Wallet&& wallet,
                                                        const Prompt& prompt) {
    std::unique_ptr<Request> request(new Request(*this, Request::Kind::Save));

    if (!validName(name) || envelope_.keys.size() >= kMaxKeys || find(name)) {
        request->fail("The wallet name is invalid or already used, or the store is full.");
        return request;
    }

    request->entry_.name = name;
    request->entry_.address = wallet.address();
    request->wallet_ = std::move(wallet);

    return start(std::move(request), kEncryptMode, prompt);
}

std::unique_ptr<WalletStore::Request> WalletStore::load(std::string_view name,
                                                        const Prompt& prompt) {
    std::unique_ptr<Request> request(new Request(*this, Request::Kind::Load));
    const StoredKey* entry = find(name);

    if (!entry) {
        request->fail("This wallet is not in the store.");
        return request;
    }

    request->entry_ = *entry;
    if (entry->keyVersion == kVersion)
        return start(std::move(request), kDecryptMode, prompt);

    // The first use of a v1 wallet authorizes re-sealing it under the v2 key.
    request->kind_ = Request::Kind::Reseal;
    return start(std::move(request), kEncryptMode, prompt);
}

// Prepares the Keystore cipher and shows the prompt that authorizes it.
std::unique_ptr<WalletStore::Request> WalletStore::start(std::unique_ptr<Request> request,
                                                         int mode, const Prompt& prompt) {
    JniScope jni(activity_);

    if (!deviceSecure(jni, activity_)) {
        request->fail("Set up a fingerprint or a screen lock (PIN, pattern or password) first.");
        return request;
    }

    // A new key is only created while no wallet depends on the current one.
    const bool mayCreate = std::none_of(
        envelope_.keys.begin(), envelope_.keys.end(),
        [](const StoredKey& key) { return key.keyVersion == kVersion; });

    auto secret = secretKey(jni, kKeyAlias, mayCreate);
    const uint8_t* iv = mode == kDecryptMode ? request->entry_.encryptedKey.data() : nullptr;
    auto cipher = initCipher(jni, secret, mode, iv);

    if (!cipher) {
        request->fail("The TEE-backed wallet key is unavailable. It is lost permanently if "
                      "the screen lock was removed; restore from the recovery words.");
        return request;
    }

    {
        const std::lock_guard lock(gPromptMutex);
        request->promptId_ = gNextPromptId++;
        gPrompts[request->promptId_] = {};
    }

    request->cipher_ = jni.env()->NewGlobalRef(cipher);
    request->cancel_ = showPrompt(jni, activity_, cipher, prompt, request->promptId_);

    if (!request->cancel_)
        request->fail("The system authentication prompt could not be shown.");

    return request;
}

bool WalletStore::erase(std::string_view name) {
    Envelope next = envelope_;
    const auto found = std::find_if(
        next.keys.begin(), next.keys.end(),
        [&](const StoredKey& key) { return key.name == name; });
    if (found == next.keys.end())
        return false;

    next.keys.erase(found);
    if (!write(next))
        return false;

    envelope_ = std::move(next);
    return true;
}
