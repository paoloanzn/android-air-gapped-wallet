#pragma once

#include <optional>
#include <string>
#include <string_view>

struct AAssetManager;

namespace call_storage {

std::optional<std::string> packaged(AAssetManager* assets,
                                     std::string& error);

// An absent override returns no value with an empty error.
std::optional<std::string> overrideText(const char* directory,
                                         std::string& error);

bool save(const char* directory, std::string_view text,
          std::string& error);

bool reset(const char* directory, std::string& error);

} // namespace call_storage
