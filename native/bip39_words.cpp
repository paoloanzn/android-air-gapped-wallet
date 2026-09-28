#include "bip39_words.hpp"

#include <android/asset_manager.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace {

std::optional<std::string> readCsv(AAssetManager* assets) {
    if (!assets)
        return std::nullopt;

    std::unique_ptr<AAsset, decltype(&AAsset_close)> asset(
        AAssetManager_open(assets, "bip39_english.csv", AASSET_MODE_BUFFER),
        AAsset_close);
    if (!asset)
        return std::nullopt;

    const off_t asset_length = AAsset_getLength(asset.get());
    if (asset_length < 5 || asset_length > 32768)
        return std::nullopt;
    const size_t csv_length = static_cast<size_t>(asset_length);
    std::string csv(csv_length, '\0');
    size_t read_length = 0;
    while (read_length < csv_length) {
        const int count = AAsset_read(asset.get(), csv.data() + read_length,
                                      csv_length - read_length);
        if (count <= 0)
            return std::nullopt;
        read_length += static_cast<size_t>(count);
    }
    return csv;
}

} // namespace

bool Bip39WordList::load(AAssetManager* assets) {
    loaded_ = false;
    pointers_.fill(nullptr);
    const auto csv = readCsv(assets);
    if (!csv || !csv->starts_with("word\n"))
        return false;

    std::array<std::array<char, 16>, 2048> parsed{};
    std::string_view remaining(*csv);
    remaining.remove_prefix(5);
    std::string_view previous;
    for (size_t row = 0; row < parsed.size(); ++row) {
        const size_t end = remaining.find('\n');
        if (end == std::string_view::npos)
            return false;
        const std::string_view word = remaining.substr(0, end);
        if (word.empty() || word.size() >= parsed[row].size() ||
            !std::all_of(word.begin(), word.end(),
                         [](char c) { return c >= 'a' && c <= 'z'; }) ||
            (row > 0 && previous >= word))
            return false;
        std::memcpy(parsed[row].data(), word.data(), word.size());
        previous = word;
        remaining.remove_prefix(end + 1);
    }
    if (!remaining.empty())
        return false;

    storage_ = parsed;
    for (size_t row = 0; row < pointers_.size(); ++row)
        pointers_[row] = storage_[row].data();
    loaded_ = true;
    return true;
}
