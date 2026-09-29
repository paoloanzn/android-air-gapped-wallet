#include "call_definitions.hpp"

#include "crypto/abi.h"
#include "crypto/selector.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace {

using Bytes = std::span<const uint8_t>;

constexpr size_t kMaxText = 65536;
constexpr size_t kMaxCalls = 256;
constexpr size_t kMaxArguments = 8;
constexpr size_t kMaxArrayElements = 1024;
constexpr size_t kMaxCalldata = 131072;

bool identifierStart(char value) {
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') || value == '_';
}

bool identifierPart(char value) {
    return identifierStart(value) || (value >= '0' && value <= '9');
}

std::string_view trim(std::string_view text) {
    const size_t first = text.find_first_not_of(" \t\r");
    if (first == std::string_view::npos)
        return {};

    const size_t last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

class Cursor {
public:
    explicit Cursor(std::string_view text) : text_(text) {}

    bool spaces() {
        const size_t start = offset_;
        while (offset_ < text_.size() &&
               (text_[offset_] == ' ' || text_[offset_] == '\t'))
            ++offset_;

        return offset_ != start;
    }

    bool take(char value) {
        spaces();
        if (offset_ >= text_.size() || text_[offset_] != value)
            return false;

        ++offset_;
        return true;
    }

    std::string_view identifier() {
        spaces();
        if (offset_ >= text_.size() || !identifierStart(text_[offset_]))
            return {};

        const size_t start = offset_++;
        while (offset_ < text_.size() && identifierPart(text_[offset_]))
            ++offset_;

        return text_.substr(start, offset_ - start);
    }

    bool done() {
        spaces();
        return offset_ == text_.size();
    }

private:
    std::string_view text_;
    size_t offset_ = 0;
};

bool parseType(std::string_view token, CallParameter& parameter) {
    if (token == "address") {
        parameter.kind = CallParameter::Kind::Address;
        parameter.canonicalType = "address";
        return true;
    }

    if (!token.starts_with("uint"))
        return false;

    unsigned width = 256;
    if (token.size() > 4) {
        width = 0;
        for (char digit : token.substr(4)) {
            if (digit < '0' || digit > '9' || width > 256)
                return false;

            width = width * 10 + static_cast<unsigned>(digit - '0');
        }
    }

    if (width < 8 || width > 256 || width % 8 != 0)
        return false;

    parameter.kind = CallParameter::Kind::Uint;
    parameter.width = width;
    parameter.canonicalType = "uint" + std::to_string(width);
    return true;
}

bool parseLine(std::string_view line, CallDefinition& call,
               std::string& error) {
    Cursor cursor(line);
    if (cursor.identifier() != "function" || !cursor.spaces()) {
        error = "expected 'function' followed by a name";
        return false;
    }

    const std::string_view name = cursor.identifier();
    if (name.empty() || name.size() > 64 || !cursor.take('(')) {
        error = "invalid function name or missing '('";
        return false;
    }

    call.signature = std::string(name) + "(";
    if (!cursor.take(')')) {
        while (true) {
            CallParameter parameter;

            const std::string_view type = cursor.identifier();
            if (!parseType(type, parameter)) {
                error = "unsupported argument type";
                return false;
            }

            if (cursor.take('[')) {
                if (!cursor.take(']')) {
                    error = "only dynamic arrays ([]) are supported";
                    return false;
                }

                parameter.array = true;
                parameter.canonicalType += "[]";
            }

            const std::string_view label = cursor.identifier();
            if (label.empty() || label.size() > 64) {
                error = "each argument needs a name";
                return false;
            }

            parameter.name = label;
            if (!call.parameters.empty())
                call.signature += ",";

            call.signature += parameter.canonicalType;
            call.parameters.push_back(std::move(parameter));
            if (call.parameters.size() > kMaxArguments) {
                error = "too many arguments";
                return false;
            }

            if (cursor.take(')'))
                break;

            if (!cursor.take(',')) {
                error = "expected ',' or ')' after argument";
                return false;
            }
        }
    }

    if (!cursor.done()) {
        error = "unexpected text after ')'";
        return false;
    }

    call.signature += ")";
    if (!compute_selector(call.signature.c_str(), call.selector.data())) {
        error = "could not compute function selector";
        return false;
    }

    return true;
}

std::string hex(Bytes bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result = "0x";
    result.reserve(2 + bytes.size() * 2);

    for (uint8_t byte : bytes) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }

    return result;
}

std::string decimal(Bytes bytes) {
    std::string digits = "0";
    for (uint8_t byte : bytes) {
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

bool readWord(Bytes bytes, size_t offset, Bytes& word) {
    if (offset > bytes.size() || bytes.size() - offset < 32)
        return false;

    word = bytes.subspan(offset, 32);
    return true;
}

bool readSize(Bytes word, size_t& value) {
    value = 0;
    for (uint8_t byte : word) {
        if (value > (std::numeric_limits<size_t>::max() - byte) / 256)
            return false;

        value = value * 256 + byte;
    }

    return true;
}

bool decodeScalar(const CallParameter& parameter, Bytes word,
                  abi_value& value, std::string& text) {
    if (parameter.kind == CallParameter::Kind::Address) {
        if (std::any_of(word.begin(), word.begin() + 12,
                        [](uint8_t byte) { return byte != 0; }))
            return false;

        value = abi_address(word.data() + 12);
        text = hex(word.last(20));
        return true;
    }

    const size_t padding = 32 - parameter.width / 8;
    if (std::any_of(word.begin(), word.begin() + padding,
                    [](uint8_t byte) { return byte != 0; }))
        return false;

    value = abi_uint_be(word.data() + padding, 32 - padding);
    text = decimal(word);
    return true;
}

bool decodeArray(const CallParameter& parameter, Bytes arguments,
                 Bytes offsetWord, std::vector<abi_value>& elements,
                 abi_value& value, std::string& text) {
    size_t offset = 0;
    if (!readSize(offsetWord, offset) || offset % 32 != 0)
        return false;

    Bytes countWord;
    if (!readWord(arguments, offset, countWord))
        return false;

    size_t count = 0;
    if (!readSize(countWord, count) || count > kMaxArrayElements)
        return false;

    const size_t start = offset + 32;
    if (count > (arguments.size() - start) / 32)
        return false;

    text = "[";
    elements.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        Bytes word = arguments.subspan(start + i * 32, 32);
        abi_value element{};
        std::string item;
        if (!decodeScalar(parameter, word, element, item))
            return false;

        if (i != 0)
            text += ", ";
        text += item;
        elements.push_back(element);
    }

    text += "]";
    value = abi_array(elements.data(), elements.size());
    return true;
}

std::optional<std::string> decodeCall(Bytes data,
                                      const CallDefinition& call) {
    if (data.size() > kMaxCalldata || data.size() < 4)
        return std::nullopt;

    Bytes arguments = data.subspan(4);
    if (arguments.size() < call.parameters.size() * 32)
        return std::nullopt;

    std::vector<abi_value> values(call.parameters.size() + 1);
    std::vector<std::vector<abi_value>> arrays(call.parameters.size());
    values[0] = abi_selector(call.selector.data());
    std::string description;

    for (size_t i = 0; i < call.parameters.size(); ++i) {
        const CallParameter& parameter = call.parameters[i];
        Bytes word = arguments.subspan(i * 32, 32);
        std::string text;

        const bool valid = parameter.array ?
            decodeArray(parameter, arguments, word, arrays[i],
                        values[i + 1], text) :
            decodeScalar(parameter, word, values[i + 1], text);
        if (!valid)
            return std::nullopt;

        description += parameter.name + ": " + text + "\n";
    }

    std::vector<uint8_t> encoded(data.size());
    size_t written = 0;
    if (abi_encode(values.data(), values.size(), encoded.data(),
                   encoded.size(), &written) != ABI_OK)
        return std::nullopt;

    if (written != data.size() || !std::equal(data.begin(), data.end(),
                                               encoded.begin()))
        return std::nullopt;

    return description;
}

} // namespace

std::optional<CallDefinitions> CallDefinitions::parse(
    std::string_view text, std::string& error) {
    error.clear();
    if (text.size() > kMaxText) {
        error = "call definitions exceed 64 KiB";
        return std::nullopt;
    }

    CallDefinitions result;
    size_t lineNumber = 0;

    while (!text.empty()) {
        ++lineNumber;
        const size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ?
               std::string_view{} : text.substr(end + 1);

        line = trim(line.substr(0, line.find('#')));
        if (line.empty())
            continue;

        CallDefinition call;
        if (!parseLine(line, call, error)) {
            error = "line " + std::to_string(lineNumber) + ": " + error;
            return std::nullopt;
        }

        for (const CallDefinition& previous : result.calls_) {
            if (previous.selector == call.selector) {
                error = "line " + std::to_string(lineNumber) +
                        ": duplicate function selector";
                return std::nullopt;
            }
        }

        result.calls_.push_back(std::move(call));
        if (result.calls_.size() > kMaxCalls) {
            error = "too many call definitions";
            return std::nullopt;
        }
    }

    if (result.calls_.empty()) {
        error = "at least one function is required";
        return std::nullopt;
    }

    return result;
}

std::optional<std::string> CallDefinitions::describe(Bytes data) const {
    if (data.size() < 4)
        return std::nullopt;

    for (const CallDefinition& call : calls_) {
        if (!std::equal(call.selector.begin(), call.selector.end(),
                        data.begin()))
            continue;

        std::string result = "Function (selector match): " +
                             call.signature + "\n";
        auto arguments = decodeCall(data, call);
        result += arguments ? *arguments : "Arguments: invalid ABI encoding\n";
        return result;
    }

    return std::nullopt;
}
