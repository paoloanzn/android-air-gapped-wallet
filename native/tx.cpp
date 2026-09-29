#include "tx.hpp"

#include "call_definitions.hpp"
#include "crypto/eth_crypto.h"

#include <algorithm>
#include <utility>

namespace {

using Bytes = std::span<const uint8_t>;

struct Item {
    bool isList = false;
    Bytes payload;
    Bytes encoded;
};

bool readLongLength(Bytes bytes, size_t& offset, size_t count, size_t& length) {
    if (count == 0 || count > sizeof(size_t) || count > bytes.size() - offset)
        return false;

    if (bytes[offset] == 0)
        return false;

    length = 0;
    for (size_t i = 0; i < count; ++i)
        length = (length << 8) | bytes[offset++];

    return length > 55;
}

bool readItem(Bytes bytes, size_t& offset, Item& item) {
    if (offset >= bytes.size())
        return false;

    const size_t start = offset;
    const uint8_t prefix = bytes[offset++];
    size_t length = 0;

    if (prefix < 0x80) {
        item = {false, bytes.subspan(start, 1), bytes.subspan(start, 1)};
        return true;
    }

    item.isList = prefix >= 0xc0;
    if (prefix <= 0xb7 || (prefix >= 0xc0 && prefix <= 0xf7)) {
        length = prefix - (item.isList ? 0xc0 : 0x80);

    } else {
        const size_t count = prefix - (item.isList ? 0xf7 : 0xb7);
        if (!readLongLength(bytes, offset, count, length))
            return false;
    }

    if (length > bytes.size() - offset)
        return false;

    if (!item.isList && prefix == 0x81 && bytes[offset] < 0x80)
        return false;

    item.payload = bytes.subspan(offset, length);
    offset += length;
    item.encoded = bytes.subspan(start, offset - start);
    return true;
}

bool readInteger(const Item& item, uint8_t* out, size_t width) {
    const Bytes bytes = item.payload;
    if (item.isList || bytes.size() > width)
        return false;

    if (!bytes.empty() && bytes.front() == 0)
        return false;

    std::fill_n(out, width, 0);
    std::copy(bytes.begin(), bytes.end(), out + width - bytes.size());
    return true;
}

bool readUint64(const Item& item, uint64_t& value) {
    std::array<uint8_t, 8> bytes{};
    if (!readInteger(item, bytes.data(), bytes.size()))
        return false;

    value = 0;
    for (uint8_t byte : bytes)
        value = (value << 8) | byte;

    return true;
}

bool readAmount(const Item& item, Transaction::Amount& value) {
    return readInteger(item, value.data(), value.size());
}

bool readAccessList(const Item& item,
                    std::vector<Transaction::AccessEntry>& entries) {
    if (!item.isList)
        return false;

    size_t offset = 0;
    while (offset < item.payload.size()) {
        Item entry;
        if (!readItem(item.payload, offset, entry) || !entry.isList)
            return false;

        size_t fieldOffset = 0;
        Item address;
        Item keys;
        if (!readItem(entry.payload, fieldOffset, address) ||
            !readItem(entry.payload, fieldOffset, keys))
            return false;

        if (fieldOffset != entry.payload.size() || address.isList ||
            address.payload.size() != 20 || !keys.isList)
            return false;

        Transaction::AccessEntry decoded;
        std::copy(address.payload.begin(), address.payload.end(),
                  decoded.address.begin());

        size_t keyOffset = 0;
        while (keyOffset < keys.payload.size()) {
            Item key;
            if (!readItem(keys.payload, keyOffset, key) || key.isList ||
                key.payload.size() != 32)
                return false;

            Transaction::Hash decodedKey{};
            std::copy(key.payload.begin(), key.payload.end(),
                      decodedKey.begin());
            decoded.storageKeys.push_back(decodedKey);
        }

        entries.push_back(std::move(decoded));
    }

    return true;
}

void appendListPrefix(std::vector<uint8_t>& out, size_t length) {
    if (length <= 55) {
        out.push_back(static_cast<uint8_t>(0xc0 + length));
        return;
    }

    std::array<uint8_t, sizeof(size_t)> bytes{};
    size_t count = 0;
    while (length != 0) {
        bytes[bytes.size() - ++count] = static_cast<uint8_t>(length);
        length >>= 8;
    }

    out.push_back(static_cast<uint8_t>(0xf7 + count));
    out.insert(out.end(), bytes.end() - count, bytes.end());
}

bool hashUnsignedFields(const std::array<Item, 12>& fields,
                        Transaction::Hash& hash) {
    size_t length = 0;
    for (size_t i = 0; i < 9; ++i)
        length += fields[i].encoded.size();

    std::vector<uint8_t> payload;
    payload.push_back(0x02);
    appendListPrefix(payload, length);

    for (size_t i = 0; i < 9; ++i) {
        Bytes encoded = fields[i].encoded;
        payload.insert(payload.end(), encoded.begin(), encoded.end());
    }

    return eth_keccak256(payload.data(), payload.size(), hash.data()) != 0;
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

std::string quantity(Bytes bytes) {
    while (!bytes.empty() && bytes.front() == 0)
        bytes = bytes.subspan(1);

    if (bytes.empty())
        return "0x0";

    std::string result = hex(bytes);
    if (result[2] == '0')
        result.erase(2, 1);

    return result;
}

void appendDecodedData(std::string& result, Bytes data,
                       const CallDefinitions* definitions) {
    if (data.empty())
        return;

    if (data.size() < 4) {
        result += "Call: data has no complete function selector\n";
        return;
    }

    Bytes selector = data.first(4);
    result += "Function selector: " + hex(selector) + "\n";

    if (definitions) {
        auto description = definitions->describe(data);
        if (description) {
            result += *description;
            return;
        }
    }

    result += definitions ? "Function: unknown selector\n" :
                            "Function: no definitions loaded\n";
}

} // namespace

std::optional<Transaction> Transaction::decode(std::span<const uint8_t> raw) {
    if (raw.size() < 2 || raw.front() != 0x02)
        return std::nullopt;

    size_t offset = 1;
    Item root;
    if (!readItem(raw, offset, root) || !root.isList || offset != raw.size())
        return std::nullopt;

    std::array<Item, 12> fields{};
    size_t fieldCount = 0;
    size_t fieldOffset = 0;

    while (fieldOffset < root.payload.size()) {
        if (fieldCount == fields.size() ||
            !readItem(root.payload, fieldOffset, fields[fieldCount]))
            return std::nullopt;

        ++fieldCount;
    }

    if (fieldCount != 9 && fieldCount != 12)
        return std::nullopt;

    Transaction tx;
    Details& details = tx.details_;
    if (!readUint64(fields[0], details.chainId) ||
        !readUint64(fields[1], details.nonce) ||
        !readAmount(fields[2], details.maxPriorityFeePerGas) ||
        !readAmount(fields[3], details.maxFeePerGas))
        return std::nullopt;

    if (!readUint64(fields[4], details.gasLimit) ||
        !readAmount(fields[6], details.value) ||
        fields[5].isList || fields[7].isList)
        return std::nullopt;

    if (details.maxPriorityFeePerGas > details.maxFeePerGas)
        return std::nullopt;

    Bytes recipient = fields[5].payload;
    if (recipient.size() != 0 && recipient.size() != 20)
        return std::nullopt;

    if (!recipient.empty()) {
        details.to.emplace();
        std::copy(recipient.begin(), recipient.end(), details.to->begin());
    }

    details.data.assign(fields[7].payload.begin(), fields[7].payload.end());
    if (!readAccessList(fields[8], details.accessList))
        return std::nullopt;

    if (fieldCount == 12) {
        uint64_t parity = 0;
        Signature signature;
        if (!readUint64(fields[9], parity) || parity > 1 ||
            !readAmount(fields[10], signature.r) ||
            !readAmount(fields[11], signature.s))
            return std::nullopt;

        signature.yParity = static_cast<uint8_t>(parity);
        details.signature = signature;
    }

    if (!hashUnsignedFields(fields, tx.signingHash_))
        return std::nullopt;

    return tx;
}

std::string Transaction::describe(const CallDefinitions* definitions) const {
    const Details& tx = details_;
    std::string result = "EIP-1559 transaction\n";
    result += "Chain ID: " + std::to_string(tx.chainId) + "\n";
    result += "Nonce: " + std::to_string(tx.nonce) + "\n";

    result += "Max priority fee per gas (wei): " +
              quantity(tx.maxPriorityFeePerGas) + "\n";
    result += "Max fee per gas (wei): " +
              quantity(tx.maxFeePerGas) + "\n";

    result += "Gas limit: " + std::to_string(tx.gasLimit) + "\n";
    result += "To: " + (tx.to ? hex(*tx.to) : "contract creation") + "\n";
    result += "Value (wei): " + quantity(tx.value) + "\n";
    result += "Data: " + hex(tx.data) + "\n";

    if (!tx.to && !tx.data.empty())
        result += "Data interpretation: contract creation bytecode\n";
    else
        appendDecodedData(result, tx.data, definitions);

    result += "Access list entries: " +
              std::to_string(tx.accessList.size()) + "\n";
    for (const AccessEntry& entry : tx.accessList) {
        result += "  " + hex(entry.address) + ": " +
                  std::to_string(entry.storageKeys.size()) + " storage keys\n";

        for (const Hash& key : entry.storageKeys)
            result += "    " + hex(key) + "\n";
    }

    result += "Signing hash: " + hex(signingHash_) + "\n";
    if (tx.signature) {
        result += "Signature y parity: " +
                  std::to_string(tx.signature->yParity) + "\n";
        result += "Signature r: " + quantity(tx.signature->r) + "\n";
        result += "Signature s: " + quantity(tx.signature->s) + "\n";

    } else {
        result += "Signature: absent\n";
    }

    return result;
}
