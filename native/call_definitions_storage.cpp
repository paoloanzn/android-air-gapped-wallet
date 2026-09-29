#include "call_definitions_storage.hpp"

#include <android/asset_manager.h>

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <memory>
#include <vector>

namespace {

constexpr size_t kMaxText = 65536;

std::string pathFor(const char* directory) {
    return std::string(directory) + "/calls.txt";
}

bool readAll(int descriptor, char* data, size_t length) {
    size_t readBytes = 0;
    while (readBytes < length) {
        const ssize_t count = ::read(descriptor, data + readBytes,
                                     length - readBytes);
        if (count < 0 && errno == EINTR)
            continue;

        if (count <= 0)
            return false;

        readBytes += static_cast<size_t>(count);
    }

    return true;
}

bool writeAll(int descriptor, std::string_view text) {
    size_t written = 0;
    while (written < text.size()) {
        const ssize_t count = ::write(descriptor, text.data() + written,
                                      text.size() - written);
        if (count < 0 && errno == EINTR)
            continue;

        if (count <= 0)
            return false;

        written += static_cast<size_t>(count);
    }

    return true;
}

void syncDirectory(const char* directory) {
    const int descriptor = ::open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor >= 0) {
        ::fsync(descriptor);
        ::close(descriptor);
    }
}

} // namespace

std::optional<std::string> call_storage::packaged(
    AAssetManager* assets, std::string& error) {
    error.clear();
    if (!assets) {
        error = "APK asset manager is unavailable";
        return std::nullopt;
    }

    std::unique_ptr<AAsset, decltype(&AAsset_close)> asset(
        AAssetManager_open(assets, "calls.txt", AASSET_MODE_BUFFER),
        AAsset_close);
    if (!asset) {
        error = "Packaged calls.txt is missing";
        return std::nullopt;
    }

    const off_t length = AAsset_getLength(asset.get());
    if (length < 0 || static_cast<size_t>(length) > kMaxText) {
        error = "Packaged calls.txt is too large";
        return std::nullopt;
    }

    std::string text(static_cast<size_t>(length), '\0');
    size_t offset = 0;

    while (offset < text.size()) {
        const int count = AAsset_read(asset.get(), text.data() + offset,
                                      text.size() - offset);
        if (count <= 0) {
            error = "Could not read packaged calls.txt";
            return std::nullopt;
        }

        offset += static_cast<size_t>(count);
    }

    return text;
}

std::optional<std::string> call_storage::overrideText(
    const char* directory, std::string& error) {
    error.clear();
    if (!directory) {
        error = "App storage is unavailable";
        return std::nullopt;
    }

    const std::string path = pathFor(directory);
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        if (errno != ENOENT)
            error = "Could not open saved calls.txt";

        return std::nullopt;
    }

    struct stat info{};
    const bool valid = ::fstat(descriptor, &info) == 0 &&
                       S_ISREG(info.st_mode) && info.st_size >= 0 &&
                       static_cast<size_t>(info.st_size) <= kMaxText;

    if (!valid) {
        ::close(descriptor);
        error = "Saved calls.txt is invalid or too large";
        return std::nullopt;
    }

    std::string text(static_cast<size_t>(info.st_size), '\0');
    const bool complete = readAll(descriptor, text.data(), text.size());
    ::close(descriptor);
    if (!complete) {
        error = "Could not read saved calls.txt";
        return std::nullopt;
    }

    return text;
}

bool call_storage::save(const char* directory, std::string_view text,
                        std::string& error) {
    error.clear();
    if (!directory || text.size() > kMaxText) {
        error = "App storage is unavailable or text exceeds 64 KiB";
        return false;
    }

    const std::string path = pathFor(directory);
    std::string pattern = path + ".tmp.XXXXXX";
    std::vector<char> temporary(pattern.begin(), pattern.end());
    temporary.push_back('\0');

    const int descriptor = ::mkstemp(temporary.data());
    if (descriptor < 0) {
        error = "Could not create a temporary calls.txt";
        return false;
    }

    const bool complete = ::fchmod(descriptor, S_IRUSR | S_IWUSR) == 0 &&
                          writeAll(descriptor, text) &&
                          ::fsync(descriptor) == 0;
    const bool closed = ::close(descriptor) == 0;

    if (!complete || !closed) {
        ::unlink(temporary.data());
        error = "Could not write calls.txt";
        return false;
    }

    if (::rename(temporary.data(), path.c_str()) != 0) {
        ::unlink(temporary.data());
        error = "Could not replace calls.txt";
        return false;
    }

    syncDirectory(directory);
    return true;
}

bool call_storage::reset(const char* directory, std::string& error) {
    error.clear();
    if (!directory) {
        error = "App storage is unavailable";
        return false;
    }

    const std::string path = pathFor(directory);
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
        error = "Could not remove saved calls.txt";
        return false;
    }

    syncDirectory(directory);
    return true;
}
