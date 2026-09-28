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
#include <span>

#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {

constexpr uint8_t kMagic = 0xaa;
constexpr uint8_t kVersion = 1;
constexpr size_t kHeaderSize = 10;
constexpr size_t kMaxKeys = 128;
constexpr size_t kMaxName = 64;
constexpr size_t kMaxFile = 32768;
constexpr char kKeyAlias[] = "airgap.wallet-store.v1";

constexpr int kEncryptMode = 1;
constexpr int kDecryptMode = 2;
constexpr int kStrongBoxSecurityLevel = 2;

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

std::vector<uint8_t> associatedData(const StoredKey& entry) {
    std::vector<uint8_t> data{kMagic, kVersion,
                              static_cast<uint8_t>(entry.name.size())};
    data.insert(data.end(), entry.name.begin(), entry.name.end());
    data.insert(data.end(), entry.address.begin(), entry.address.end());
    return data;
}

jobject generateStrongBoxKey(JniScope& jni) {
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

    if (!jni.object(builder, "setIsStrongBoxBacked",
                    "(Z)Landroid/security/keystore/KeyGenParameterSpec$Builder;",
                    JNI_TRUE))
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

bool isStrongBoxKey(JniScope& jni, jobject key) {
    if (android_get_device_api_level() < 31)
        return true;

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
    return info && jni.integer(info, "getSecurityLevel", "()I") ==
                       kStrongBoxSecurityLevel;
}

jobject secretKey(JniScope& jni, bool create) {
    if (android_get_device_api_level() < 28)
        return nullptr;

    auto provider = jni.string("AndroidKeyStore");
    auto alias = jni.string(kKeyAlias);
    if (!provider || !alias)
        return nullptr;

    auto store = jni.staticObject(
        "java/security/KeyStore", "getInstance",
        "(Ljava/lang/String;)Ljava/security/KeyStore;", provider);
    if (!store || !jni.call(store, "load",
                            "(Ljava/io/InputStream;[C)V", nullptr, nullptr))
        return nullptr;

    auto key = jni.object(store, "getKey",
                          "(Ljava/lang/String;[C)Ljava/security/Key;",
                          alias, nullptr);
    if (!jni.ready())
        return nullptr;

    if (!key && create)
        key = generateStrongBoxKey(jni);

    return key && isStrongBoxKey(jni, key) ? key : nullptr;
}

jobject newCipher(JniScope& jni) {
    auto transformation = jni.string("AES/GCM/NoPadding");
    if (!transformation)
        return nullptr;

    return jni.staticObject(
        "javax/crypto/Cipher", "getInstance",
        "(Ljava/lang/String;)Ljavax/crypto/Cipher;", transformation);
}

bool encryptKey(ANativeActivity* activity, const Wallet::PrivateKey& key,
                StoredKey& entry, bool mayCreateKey) {
    JniScope jni(activity);
    auto secret = secretKey(jni, mayCreateKey);
    auto cipher = newCipher(jni);
    if (!secret || !cipher)
        return false;

    if (!jni.call(cipher, "init", "(ILjava/security/Key;)V",
                  kEncryptMode, secret))
        return false;

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

bool decryptKey(ANativeActivity* activity, const StoredKey& entry,
                Wallet::PrivateKey& output) {
    JniScope jni(activity);
    auto secret = secretKey(jni, false);
    auto cipher = newCipher(jni);
    if (!secret || !cipher)
        return false;

    auto iv = jni.bytes(entry.encryptedKey.data(), 12);
    if (!iv)
        return false;

    auto params = jni.make("javax/crypto/spec/GCMParameterSpec",
                           "(I[B)V", 128, iv);
    if (!params || !jni.call(
            cipher, "init",
            "(ILjava/security/Key;Ljava/security/spec/AlgorithmParameterSpec;)V",
            kDecryptMode, secret, params))
        return false;

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

        size += 1 + entry.encryptedKey.size() +
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
        bytes.insert(bytes.end(), entry.encryptedKey.begin(),
                     entry.encryptedKey.end());

        bytes.insert(bytes.end(), entry.address.begin(), entry.address.end());
        bytes.insert(bytes.end(), entry.name.begin(), entry.name.end());
    }

    return bytes;
}

std::optional<Envelope> decode(std::span<const uint8_t> bytes) {
    if (bytes.size() < kHeaderSize || bytes.size() > kMaxFile ||
        bytes[0] != kMagic || bytes[1] != kVersion)
        return std::nullopt;

    const uint32_t count = read32(bytes, 2);
    const uint32_t size = read32(bytes, 6);
    if (count > kMaxKeys || size != bytes.size())
        return std::nullopt;

    Envelope envelope;
    envelope.keys.reserve(count);
    size_t offset = kHeaderSize;

    for (uint32_t index = 0; index < count; ++index) {
        if (bytes.size() - offset < 81)
            return std::nullopt;

        const size_t nameSize = bytes[offset++];
        if (nameSize == 0 || nameSize > kMaxName ||
            bytes.size() - offset < 80 + nameSize)
            return std::nullopt;

        StoredKey entry;
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

bool WalletStore::save(std::string_view name, const Wallet& wallet) {
    if (!validName(name) || envelope_.keys.size() >= kMaxKeys)
        return false;

    const auto existing = std::find_if(
        envelope_.keys.begin(), envelope_.keys.end(),
        [&](const StoredKey& key) { return key.name == name; });
    if (existing != envelope_.keys.end())
        return false;

    StoredKey entry;
    entry.name = name;
    entry.address = wallet.address();
    if (!encryptKey(activity_, wallet.privateKey_, entry,
                    envelope_.keys.empty()))
        return false;

    Envelope next = envelope_;
    next.keys.push_back(std::move(entry));
    if (!write(next))
        return false;

    envelope_ = std::move(next);
    return true;
}

std::optional<Wallet> WalletStore::load(std::string_view name) const {
    const auto found = std::find_if(
        envelope_.keys.begin(), envelope_.keys.end(),
        [&](const StoredKey& key) { return key.name == name; });
    if (found == envelope_.keys.end())
        return std::nullopt;

    Wallet::PrivateKey privateKey{};
    if (!decryptKey(activity_, *found, privateKey)) {
        OPENSSL_cleanse(privateKey.data(), privateKey.size());
        return std::nullopt;
    }

    auto wallet = Wallet::load(std::move(privateKey));
    if (!wallet || wallet->address() != found->address)
        return std::nullopt;

    return wallet;
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
