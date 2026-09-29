#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct CallParameter {
    enum class Kind { Address, Uint };

    Kind kind = Kind::Address;
    std::string name;
    std::string canonicalType;
    unsigned width = 0;
    bool array = false;
};

struct CallDefinition {
    std::string signature;
    std::array<uint8_t, 4> selector{};
    std::vector<CallParameter> parameters;
};

class CallDefinitions {
public:
    // One declaration per line. Supports address, uint, uint8..uint256,
    // and dynamic arrays of those types. Blank lines and # comments are valid.
    static std::optional<CallDefinitions> parse(std::string_view text,
                                                 std::string& error);

    // Returns no value when the selector is unknown. A selector match alone
    // cannot prove the target contract implements this function.
    std::optional<std::string> describe(std::span<const uint8_t> data) const;

    size_t size() const noexcept { return calls_.size(); }

private:
    std::vector<CallDefinition> calls_;
};
