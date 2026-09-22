/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "update_core.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>
#include <utility>

namespace melee::vita::update {
namespace {

bool parse_u32_component(
    std::string_view text, std::size_t& position, std::uint32_t& value) {
    const std::size_t start = position;
    while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
        ++position;
    }
    if (position == start || (position - start > 1 && text[start] == '0')) {
        return false;
    }
    const auto result =
        std::from_chars(text.data() + start, text.data() + position, value);
    return result.ec == std::errc() && result.ptr == text.data() + position;
}

bool valid_identifier_char(char value) {
    return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') ||
        (value >= 'a' && value <= 'z') || value == '-';
}

bool validate_identifiers(
    std::string_view identifiers, bool reject_numeric_leading_zero) {
    if (identifiers.empty()) {
        return false;
    }
    std::size_t start = 0;
    while (start < identifiers.size()) {
        const std::size_t end = identifiers.find('.', start);
        const std::size_t length =
            (end == std::string_view::npos ? identifiers.size() : end) - start;
        if (length == 0) {
            return false;
        }
        bool numeric = true;
        for (std::size_t index = start; index < start + length; ++index) {
            if (!valid_identifier_char(identifiers[index])) {
                return false;
            }
            numeric = numeric && identifiers[index] >= '0' && identifiers[index] <= '9';
        }
        if (reject_numeric_leading_zero && numeric && length > 1 && identifiers[start] == '0') {
            return false;
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return true;
}

bool identifier_is_numeric(std::string_view identifier) {
    if (identifier.empty()) {
        return false;
    }
    return std::all_of(identifier.begin(), identifier.end(),
        [](char value) { return value >= '0' && value <= '9'; });
}

int compare_prerelease(std::string_view left, std::string_view right) {
    if (left.empty() || right.empty()) {
        if (left.empty() && right.empty()) {
            return 0;
        }
        return left.empty() ? 1 : -1;
    }

    std::size_t left_position = 0;
    std::size_t right_position = 0;
    for (;;) {
        const std::size_t left_end = left.find('.', left_position);
        const std::size_t right_end = right.find('.', right_position);
        const std::string_view left_part = left.substr(left_position,
            (left_end == std::string_view::npos ? left.size() : left_end) - left_position);
        const std::string_view right_part = right.substr(right_position,
            (right_end == std::string_view::npos ? right.size() : right_end) - right_position);
        const bool left_numeric = identifier_is_numeric(left_part);
        const bool right_numeric = identifier_is_numeric(right_part);

        if (left_numeric != right_numeric) {
            return left_numeric ? -1 : 1;
        }
        if (left_numeric) {
            if (left_part.size() != right_part.size()) {
                return left_part.size() < right_part.size() ? -1 : 1;
            }
        }
        if (left_part != right_part) {
            return left_part < right_part ? -1 : 1;
        }

        const bool left_done = left_end == std::string_view::npos;
        const bool right_done = right_end == std::string_view::npos;
        if (left_done || right_done) {
            if (left_done && right_done) {
                return 0;
            }
            return left_done ? -1 : 1;
        }
        left_position = left_end + 1;
        right_position = right_end + 1;
    }
}

enum class JsonType { Null, Boolean, Number, String, Array, Object };

struct JsonValue {
    JsonType type = JsonType::Null;
    bool boolean = false;
    std::string text;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;

    const JsonValue* find(std::string_view key) const {
        for (const auto& entry : object) {
            if (entry.first == key) {
                return &entry.second;
            }
        }
        return nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    bool parse(JsonValue& value, std::string& error) {
        if (!parse_value(value, 0, error)) {
            return false;
        }
        skip_space();
        if (position_ != input_.size()) {
            error = "JSON contains trailing data";
            return false;
        }
        return true;
    }

private:
    static bool append_utf8(std::string& output, std::uint32_t codepoint) {
        if (codepoint <= 0x7f) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7ff) {
            output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else if (codepoint <= 0xffff) {
            output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else if (codepoint <= 0x10ffff) {
            output.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else {
            return false;
        }
        return true;
    }

    static int hex_digit(char value) {
        if (value >= '0' && value <= '9') {
            return value - '0';
        }
        if (value >= 'a' && value <= 'f') {
            return value - 'a' + 10;
        }
        if (value >= 'A' && value <= 'F') {
            return value - 'A' + 10;
        }
        return -1;
    }

    bool parse_hex_quad(std::uint32_t& value) {
        if (position_ + 4 > input_.size()) {
            return false;
        }
        value = 0;
        for (unsigned index = 0; index < 4; ++index) {
            const int digit = hex_digit(input_[position_++]);
            if (digit < 0) {
                return false;
            }
            value = (value << 4) | static_cast<std::uint32_t>(digit);
        }
        return true;
    }

    bool parse_string(std::string& output, std::string& error) {
        if (position_ >= input_.size() || input_[position_] != '"') {
            error = "Expected JSON string";
            return false;
        }
        ++position_;
        output.clear();
        while (position_ < input_.size()) {
            const unsigned char value = static_cast<unsigned char>(input_[position_++]);
            if (value == '"') {
                return true;
            }
            if (value < 0x20) {
                error = "JSON string contains an unescaped control character";
                return false;
            }
            if (value != '\\') {
                output.push_back(static_cast<char>(value));
                continue;
            }
            if (position_ >= input_.size()) {
                error = "JSON string ends after an escape";
                return false;
            }
            const char escaped = input_[position_++];
            switch (escaped) {
            case '"':
            case '\\':
            case '/':
                output.push_back(escaped);
                break;
            case 'b':
                output.push_back('\b');
                break;
            case 'f':
                output.push_back('\f');
                break;
            case 'n':
                output.push_back('\n');
                break;
            case 'r':
                output.push_back('\r');
                break;
            case 't':
                output.push_back('\t');
                break;
            case 'u': {
                std::uint32_t codepoint = 0;
                if (!parse_hex_quad(codepoint)) {
                    error = "JSON string contains an invalid Unicode escape";
                    return false;
                }
                if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                    if (position_ + 2 > input_.size() || input_[position_] != '\\' ||
                        input_[position_ + 1] != 'u')
                    {
                        error = "JSON string contains an unpaired high surrogate";
                        return false;
                    }
                    position_ += 2;
                    std::uint32_t low = 0;
                    if (!parse_hex_quad(low) || low < 0xdc00 || low > 0xdfff) {
                        error = "JSON string contains an invalid surrogate pair";
                        return false;
                    }
                    codepoint =
                        0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
                } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
                    error = "JSON string contains an unpaired low surrogate";
                    return false;
                }
                if (!append_utf8(output, codepoint)) {
                    error = "JSON string contains an invalid codepoint";
                    return false;
                }
                break;
            }
            default:
                error = "JSON string contains an invalid escape";
                return false;
            }
        }
        error = "JSON string is not terminated";
        return false;
    }

    bool parse_number(std::string& output, std::string& error) {
        const std::size_t start = position_;
        if (position_ < input_.size() && input_[position_] == '-') {
            ++position_;
        }
        if (position_ >= input_.size()) {
            error = "JSON number is incomplete";
            return false;
        }
        if (input_[position_] == '0') {
            ++position_;
            if (position_ < input_.size() && input_[position_] >= '0' &&
                input_[position_] <= '9')
            {
                error = "JSON number has a leading zero";
                return false;
            }
        } else {
            const std::size_t integer_start = position_;
            while (position_ < input_.size() && input_[position_] >= '0' &&
                input_[position_] <= '9')
            {
                ++position_;
            }
            if (integer_start == position_) {
                error = "JSON number has no integer part";
                return false;
            }
        }
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            const std::size_t fraction_start = position_;
            while (position_ < input_.size() && input_[position_] >= '0' &&
                input_[position_] <= '9')
            {
                ++position_;
            }
            if (fraction_start == position_) {
                error = "JSON number has no fractional digits";
                return false;
            }
        }
        if (position_ < input_.size() &&
            (input_[position_] == 'e' || input_[position_] == 'E'))
        {
            ++position_;
            if (position_ < input_.size() &&
                (input_[position_] == '+' || input_[position_] == '-'))
            {
                ++position_;
            }
            const std::size_t exponent_start = position_;
            while (position_ < input_.size() && input_[position_] >= '0' &&
                input_[position_] <= '9')
            {
                ++position_;
            }
            if (exponent_start == position_) {
                error = "JSON number has no exponent digits";
                return false;
            }
        }
        output.assign(input_.substr(start, position_ - start));
        return true;
    }

    bool parse_value(JsonValue& value, unsigned depth, std::string& error) {
        if (depth > 16) {
            error = "JSON nesting exceeds the limit";
            return false;
        }
        skip_space();
        if (position_ >= input_.size()) {
            error = "JSON ends before a value";
            return false;
        }
        if (++value_count_ > 4096) {
            error = "JSON contains too many values";
            return false;
        }

        const char token = input_[position_];
        if (token == '"') {
            value.type = JsonType::String;
            return parse_string(value.text, error);
        }
        if (token == '{') {
            value.type = JsonType::Object;
            ++position_;
            skip_space();
            if (position_ < input_.size() && input_[position_] == '}') {
                ++position_;
                return true;
            }
            for (;;) {
                std::string key;
                if (!parse_string(key, error)) {
                    return false;
                }
                if (value.find(key) != nullptr) {
                    error = "JSON object contains a duplicate key";
                    return false;
                }
                skip_space();
                if (position_ >= input_.size() || input_[position_] != ':') {
                    error = "JSON object key is missing ':'";
                    return false;
                }
                ++position_;
                JsonValue child;
                if (!parse_value(child, depth + 1, error)) {
                    return false;
                }
                value.object.emplace_back(std::move(key), std::move(child));
                skip_space();
                if (position_ >= input_.size()) {
                    error = "JSON object is not terminated";
                    return false;
                }
                if (input_[position_] == '}') {
                    ++position_;
                    return true;
                }
                if (input_[position_] != ',') {
                    error = "JSON object entries are not comma separated";
                    return false;
                }
                ++position_;
                skip_space();
            }
        }
        if (token == '[') {
            value.type = JsonType::Array;
            ++position_;
            skip_space();
            if (position_ < input_.size() && input_[position_] == ']') {
                ++position_;
                return true;
            }
            for (;;) {
                JsonValue child;
                if (!parse_value(child, depth + 1, error)) {
                    return false;
                }
                value.array.push_back(std::move(child));
                skip_space();
                if (position_ >= input_.size()) {
                    error = "JSON array is not terminated";
                    return false;
                }
                if (input_[position_] == ']') {
                    ++position_;
                    return true;
                }
                if (input_[position_] != ',') {
                    error = "JSON array entries are not comma separated";
                    return false;
                }
                ++position_;
            }
        }
        if (input_.substr(position_, 4) == "true") {
            position_ += 4;
            value.type = JsonType::Boolean;
            value.boolean = true;
            return true;
        }
        if (input_.substr(position_, 5) == "false") {
            position_ += 5;
            value.type = JsonType::Boolean;
            value.boolean = false;
            return true;
        }
        if (input_.substr(position_, 4) == "null") {
            position_ += 4;
            value.type = JsonType::Null;
            return true;
        }
        if (token == '-' || (token >= '0' && token <= '9')) {
            value.type = JsonType::Number;
            return parse_number(value.text, error);
        }
        error = "JSON contains an unexpected token";
        return false;
    }

    void skip_space() {
        while (position_ < input_.size() &&
            (input_[position_] == ' ' || input_[position_] == '\t' ||
                input_[position_] == '\n' || input_[position_] == '\r'))
        {
            ++position_;
        }
    }

    std::string_view input_;
    std::size_t position_ = 0;
    std::size_t value_count_ = 0;
};

bool get_required(const JsonValue& object, std::string_view key, JsonType type,
    const JsonValue*& value, std::string& error) {
    value = object.find(key);
    if (value == nullptr) {
        error = "GitHub release is missing '" + std::string(key) + "'";
        return false;
    }
    if (value->type != type) {
        error = "GitHub release field '" + std::string(key) + "' has the wrong type";
        return false;
    }
    return true;
}

bool parse_json_u64(const JsonValue& value, std::uint64_t& output) {
    if (value.type != JsonType::Number || value.text.empty() || value.text.front() == '-' ||
        value.text.find_first_of(".eE") != std::string::npos)
    {
        return false;
    }
    const auto result =
        std::from_chars(value.text.data(), value.text.data() + value.text.size(), output);
    return result.ec == std::errc() &&
        result.ptr == value.text.data() + value.text.size();
}

bool safe_asset_name(std::string_view name) {
    if (name.empty() || name.size() > 128 || name == "." || name == "..") {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char value) {
        return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') ||
            (value >= 'a' && value <= 'z') || value == '-' || value == '_' || value == '.';
    });
}

bool decode_hex(std::string_view text, std::uint8_t* output, std::size_t output_size) {
    if (text.size() != output_size * 2) {
        return false;
    }
    auto digit = [](char value) -> int {
        if (value >= '0' && value <= '9') {
            return value - '0';
        }
        if (value >= 'a' && value <= 'f') {
            return value - 'a' + 10;
        }
        if (value >= 'A' && value <= 'F') {
            return value - 'A' + 10;
        }
        return -1;
    };
    for (std::size_t index = 0; index < output_size; ++index) {
        const int high = digit(text[index * 2]);
        const int low = digit(text[index * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        output[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

bool split_manifest_lines(
    std::string_view text, std::array<std::string_view, 6>& lines) {
    std::size_t start = 0;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const std::size_t newline = text.find('\n', start);
        const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
        std::string_view line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        lines[index] = line;
        if (newline == std::string_view::npos) {
            return index + 1 == lines.size();
        }
        start = newline + 1;
    }
    return start == text.size();
}

bool parse_manifest_field(
    std::string_view line, std::string_view key, std::string_view& value) {
    if (!line.starts_with(key) || line.size() <= key.size() ||
        line[key.size()] != '=')
    {
        return false;
    }
    value = line.substr(key.size() + 1);
    return !value.empty() && value.front() != ' ' && value.back() != ' ';
}

const ReleaseAsset* find_asset(
    const ReleaseMetadata& release, std::string_view name) {
    const ReleaseAsset* found = nullptr;
    for (const ReleaseAsset& asset : release.assets) {
        if (asset.name == name) {
            if (found != nullptr) {
                return nullptr;
            }
            found = &asset;
        }
    }
    return found;
}

bool valid_release_url(
    const ReleaseAsset& asset, std::string_view release_tag) {
    std::string expected(kReleaseDownloadPrefix);
    expected.append(release_tag);
    expected.push_back('/');
    expected.append(asset.name);
    return asset.download_url == expected;
}

void copy_error(char* destination, std::size_t destination_size, std::string_view error) {
    if (destination == nullptr || destination_size == 0) {
        return;
    }
    const std::size_t length = std::min(destination_size - 1, error.size());
    std::memcpy(destination, error.data(), length);
    destination[length] = '\0';
}

constexpr std::array<std::uint32_t, 64> kSha256Constants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

std::uint32_t rotate_right(std::uint32_t value, unsigned count) {
    return (value >> count) | (value << (32 - count));
}

}  // namespace

bool parse_semver(std::string_view text, SemVer& version, std::string& error) {
    version = {};
    error.clear();
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) {
        text.remove_prefix(1);
    }
    if (text.empty() || text.size() > 128) {
        error = "Semantic version is empty or too long";
        return false;
    }

    std::size_t position = 0;
    if (!parse_u32_component(text, position, version.major) ||
        position >= text.size() || text[position++] != '.' ||
        !parse_u32_component(text, position, version.minor) ||
        position >= text.size() || text[position++] != '.' ||
        !parse_u32_component(text, position, version.patch))
    {
        error = "Semantic version must contain strict major.minor.patch components";
        return false;
    }

    if (position < text.size() && text[position] == '-') {
        const std::size_t start = ++position;
        while (position < text.size() && text[position] != '+') {
            ++position;
        }
        version.prerelease.assign(text.substr(start, position - start));
        if (!validate_identifiers(version.prerelease, true)) {
            error = "Semantic version has an invalid prerelease component";
            return false;
        }
    }
    if (position < text.size() && text[position] == '+') {
        const std::size_t start = ++position;
        position = text.size();
        version.build.assign(text.substr(start));
        if (!validate_identifiers(version.build, false)) {
            error = "Semantic version has invalid build metadata";
            return false;
        }
    }
    if (position != text.size()) {
        error = "Semantic version contains trailing characters";
        return false;
    }
    return true;
}

int compare_semver(const SemVer& left, const SemVer& right) {
    if (left.major != right.major) {
        return left.major < right.major ? -1 : 1;
    }
    if (left.minor != right.minor) {
        return left.minor < right.minor ? -1 : 1;
    }
    if (left.patch != right.patch) {
        return left.patch < right.patch ? -1 : 1;
    }
    return compare_prerelease(left.prerelease, right.prerelease);
}

bool parse_github_release(
    std::string_view json, ReleaseMetadata& release, std::string& error) {
    release = {};
    error.clear();
    if (json.empty() || json.size() > kMaxReleaseMetadataBytes) {
        error = "GitHub release metadata is empty or exceeds 64 KiB";
        return false;
    }

    JsonValue root;
    JsonParser parser(json);
    if (!parser.parse(root, error)) {
        return false;
    }
    if (root.type != JsonType::Object) {
        error = "GitHub latest-release response must be a JSON object";
        return false;
    }

    const JsonValue* tag = nullptr;
    const JsonValue* draft = nullptr;
    const JsonValue* prerelease = nullptr;
    const JsonValue* published = nullptr;
    const JsonValue* assets = nullptr;
    if (!get_required(root, "tag_name", JsonType::String, tag, error) ||
        !get_required(root, "draft", JsonType::Boolean, draft, error) ||
        !get_required(root, "prerelease", JsonType::Boolean, prerelease, error) ||
        !get_required(root, "published_at", JsonType::String, published, error) ||
        !get_required(root, "assets", JsonType::Array, assets, error))
    {
        return false;
    }
    if (draft->boolean) {
        error = "Draft releases are not eligible for updates";
        return false;
    }
    if (tag->text.empty() || tag->text.size() > 64 || published->text.empty() ||
        published->text.size() > 64)
    {
        error = "GitHub release tag or publication timestamp is invalid";
        return false;
    }
    SemVer release_version;
    if (!parse_semver(tag->text, release_version, error)) {
        error = "GitHub release tag is not a semantic version: " + error;
        return false;
    }

    release.tag_name = tag->text;
    release.published_at = published->text;
    release.prerelease = prerelease->boolean;
    if (const JsonValue* name = root.find("name");
        name != nullptr && name->type != JsonType::Null)
    {
        if (name->type != JsonType::String || name->text.size() > 128 ||
            name->text.find('\0') != std::string::npos)
        {
            error = "GitHub release name is invalid";
            return false;
        }
        release.name = name->text;
    }
    if (release.name.empty()) {
        release.name = release.tag_name;
    }
    if (const JsonValue* body = root.find("body");
        body != nullptr && body->type != JsonType::Null)
    {
        if (body->type != JsonType::String || body->text.size() > kMaxReleaseNotesBytes ||
            body->text.find('\0') != std::string::npos)
        {
            error = "GitHub release notes exceed 8 KiB";
            return false;
        }
        release.notes = body->text;
    }
    if (assets->array.size() > 64) {
        error = "GitHub release contains too many assets";
        return false;
    }

    for (const JsonValue& item : assets->array) {
        if (item.type != JsonType::Object) {
            error = "GitHub release asset is not an object";
            return false;
        }
        const JsonValue* name = nullptr;
        const JsonValue* url = nullptr;
        const JsonValue* size = nullptr;
        const JsonValue* state = nullptr;
        if (!get_required(item, "name", JsonType::String, name, error) ||
            !get_required(item, "browser_download_url", JsonType::String, url, error) ||
            !get_required(item, "size", JsonType::Number, size, error) ||
            !get_required(item, "state", JsonType::String, state, error))
        {
            return false;
        }
        ReleaseAsset asset;
        asset.name = name->text;
        asset.download_url = url->text;
        if (!safe_asset_name(asset.name) || asset.download_url.size() > 1024 ||
            !asset.download_url.starts_with("https://") || state->text != "uploaded" ||
            !parse_json_u64(*size, asset.size) || asset.size == 0)
        {
            error = "GitHub release asset has unsafe or incomplete metadata";
            return false;
        }
        if (find_asset(release, asset.name) != nullptr) {
            error = "GitHub release contains duplicate asset names";
            return false;
        }
        release.assets.push_back(std::move(asset));
    }
    return true;
}

bool validate_release_asset_url(
    const ReleaseAsset& asset, std::string_view release_tag) {
    return valid_release_url(asset, release_tag);
}

bool parse_signed_manifest(
    std::string_view text, SignedManifest& manifest, std::string& error) {
    manifest = {};
    error.clear();
    if (text.empty() || text.size() > kMaxManifestBytes) {
        error = "Update manifest is empty or exceeds 4 KiB";
        return false;
    }

    std::array<std::string_view, 6> lines{};
    if (!split_manifest_lines(text, lines)) {
        error = "Update manifest must contain exactly six lines";
        return false;
    }
    std::array<std::string_view, 6> values{};
    constexpr std::array<std::string_view, 6> keys = {
        "format", "version", "asset", "size", "sha256", "signature"};
    for (std::size_t index = 0; index < keys.size(); ++index) {
        if (!parse_manifest_field(lines[index], keys[index], values[index])) {
            error = "Update manifest fields are missing, reordered, or padded";
            return false;
        }
    }
    if (values[0] != "1") {
        error = "Update manifest format is unsupported";
        return false;
    }
    manifest.format = 1;
    manifest.version_text.assign(values[1]);
    if (!parse_semver(values[1], manifest.version, error)) {
        error = "Update manifest version is invalid: " + error;
        return false;
    }
    if (!safe_asset_name(values[2]) || values[2] == kManifestAssetName) {
        error = "Update manifest asset name is unsafe";
        return false;
    }
    manifest.asset_name.assign(values[2]);

    const auto size_result = std::from_chars(
        values[3].data(), values[3].data() + values[3].size(), manifest.asset_size);
    if (size_result.ec != std::errc() ||
        size_result.ptr != values[3].data() + values[3].size() ||
        manifest.asset_size == 0 || manifest.asset_size > kMaxPackageBytes)
    {
        error = "Update manifest package size is invalid";
        return false;
    }
    if (!decode_hex(values[4], manifest.sha256.data(), manifest.sha256.size())) {
        error = "Update manifest SHA-256 digest is invalid";
        return false;
    }
    if (!decode_hex(values[5], manifest.signature.data(), manifest.signature.size())) {
        error = "Update manifest Ed25519 signature is invalid";
        return false;
    }

    manifest.signed_payload = "format=1\nversion=" + manifest.version_text +
        "\nasset=" + manifest.asset_name + "\nsize=" +
        std::to_string(manifest.asset_size) + "\nsha256=" +
        sha256_hex(manifest.sha256) + "\n";
    return true;
}

bool verify_manifest_signature(const SignedManifest& manifest,
    const std::array<std::uint8_t, 32>& public_key, SignatureVerifier verifier,
    std::string& error) {
    error.clear();
    if (verifier == nullptr) {
        error = "No Ed25519 verifier is available";
        return false;
    }
    if (verifier(public_key.data(), manifest.signature.data(),
            reinterpret_cast<const std::uint8_t*>(manifest.signed_payload.data()),
            manifest.signed_payload.size()) != 1)
    {
        error = "Update manifest signature verification failed";
        return false;
    }
    return true;
}

Sha256::Sha256()
    : state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f,
          0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

void Sha256::transform(const std::uint8_t block[64]) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
        words[index] = (static_cast<std::uint32_t>(block[index * 4]) << 24) |
            (static_cast<std::uint32_t>(block[index * 4 + 1]) << 16) |
            (static_cast<std::uint32_t>(block[index * 4 + 2]) << 8) |
            static_cast<std::uint32_t>(block[index * 4 + 3]);
    }
    for (std::size_t index = 16; index < words.size(); ++index) {
        const std::uint32_t s0 = rotate_right(words[index - 15], 7) ^
            rotate_right(words[index - 15], 18) ^ (words[index - 15] >> 3);
        const std::uint32_t s1 = rotate_right(words[index - 2], 17) ^
            rotate_right(words[index - 2], 19) ^ (words[index - 2] >> 10);
        words[index] = words[index - 16] + s0 + words[index - 7] + s1;
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];
    for (std::size_t index = 0; index < words.size(); ++index) {
        const std::uint32_t sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^
            rotate_right(e, 25);
        const std::uint32_t choice = (e & f) ^ (~e & g);
        const std::uint32_t temporary1 =
            h + sum1 + choice + kSha256Constants[index] + words[index];
        const std::uint32_t sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^
            rotate_right(a, 22);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temporary2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(const void* data, std::size_t size) {
    if (finished_ || (data == nullptr && size != 0)) {
        return;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    total_size_ += size;
    while (size > 0) {
        const std::size_t copy_size = std::min(size, buffer_.size() - buffered_);
        std::memcpy(buffer_.data() + buffered_, bytes, copy_size);
        buffered_ += copy_size;
        bytes += copy_size;
        size -= copy_size;
        if (buffered_ == buffer_.size()) {
            transform(buffer_.data());
            buffered_ = 0;
        }
    }
}

std::array<std::uint8_t, 32> Sha256::finish() {
    if (!finished_) {
        const std::uint64_t bit_size = total_size_ * 8;
        buffer_[buffered_++] = 0x80;
        if (buffered_ > 56) {
            std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_),
                buffer_.end(), 0);
            transform(buffer_.data());
            buffered_ = 0;
        }
        std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_),
            buffer_.begin() + 56, 0);
        for (unsigned index = 0; index < 8; ++index) {
            buffer_[63 - index] =
                static_cast<std::uint8_t>(bit_size >> (index * 8));
        }
        transform(buffer_.data());
        finished_ = true;
    }

    std::array<std::uint8_t, 32> digest{};
    for (std::size_t index = 0; index < state_.size(); ++index) {
        digest[index * 4] = static_cast<std::uint8_t>(state_[index] >> 24);
        digest[index * 4 + 1] = static_cast<std::uint8_t>(state_[index] >> 16);
        digest[index * 4 + 2] = static_cast<std::uint8_t>(state_[index] >> 8);
        digest[index * 4 + 3] = static_cast<std::uint8_t>(state_[index]);
    }
    return digest;
}

std::string sha256_hex(const std::array<std::uint8_t, 32>& digest) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.resize(digest.size() * 2);
    for (std::size_t index = 0; index < digest.size(); ++index) {
        output[index * 2] = digits[digest[index] >> 4];
        output[index * 2 + 1] = digits[digest[index] & 0x0f];
    }
    return output;
}

bool validate_release_for_staging(const ReleaseMetadata& release,
    const SignedManifest& manifest, std::string_view current_version,
    std::uint64_t installed_size, std::uint64_t available_bytes,
    const std::array<std::uint8_t, 32>& public_key, SignatureVerifier verifier,
    StagingDecision& decision, std::string& error) {
    decision = {};
    error.clear();
    if (!verify_manifest_signature(manifest, public_key, verifier, error)) {
        return false;
    }

    SemVer current;
    SemVer released;
    if (!parse_semver(current_version, current, error)) {
        error = "Compiled game version is invalid: " + error;
        return false;
    }
    if (!parse_semver(release.tag_name, released, error)) {
        error = "Release version is invalid: " + error;
        return false;
    }
    if (compare_semver(current, released) >= 0) {
        error = "Published release is not newer than the compiled game";
        return false;
    }
    if (compare_semver(released, manifest.version) != 0) {
        error = "Manifest version does not match the published release";
        return false;
    }

    decision.package_asset = find_asset(release, manifest.asset_name);
    decision.manifest_asset = find_asset(release, kManifestAssetName);
    if (decision.package_asset == nullptr || decision.manifest_asset == nullptr) {
        error = "Published release is missing a unique package or manifest asset";
        return false;
    }
    if (decision.package_asset->size != manifest.asset_size ||
        manifest.asset_size > kMaxPackageBytes ||
        decision.manifest_asset->size > kMaxManifestBytes)
    {
        error = "Published asset sizes do not satisfy the signed limits";
        return false;
    }
    if (!valid_release_url(*decision.package_asset, release.tag_name) ||
        !valid_release_url(*decision.manifest_asset, release.tag_name))
    {
        error = "Published asset URL is outside the expected GitHub release";
        return false;
    }
    if (installed_size >
        std::numeric_limits<std::uint64_t>::max() - manifest.asset_size -
            kMaxExtractedPackageBytes - kStorageReserveBytes)
    {
        error = "Storage requirement overflow";
        return false;
    }
    decision.required_free_bytes =
        installed_size + manifest.asset_size + kMaxExtractedPackageBytes +
        kStorageReserveBytes;
    if (available_bytes < decision.required_free_bytes) {
        error = "Insufficient storage for staging, backup, and rollback reserve";
        return false;
    }
    return true;
}

bool build_transaction_plan(
    std::string_view title_id, TransactionPlan& plan, std::string& error) {
    plan = {};
    error.clear();
    if (title_id.size() != 9 ||
        !std::all_of(title_id.begin(), title_id.end(), [](char value) {
            return (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9');
        }))
    {
        error = "Vita title ID must contain nine uppercase alphanumeric characters";
        return false;
    }

    plan.requires_external_process = true;
    plan.live_path = "ux0:/app/" + std::string(title_id);
    plan.staged_path = std::string(kStagingRoot) + "new-package";
    plan.backup_path = std::string(kStagingRoot) + "backup-package";
    plan.journal_path = std::string(kStagingRoot) + "transaction.pending";
    plan.protected_paths = {
        "ux0:/data/melee/save/", "ux0:/data/melee/shadercache/"};

    plan.commit_steps = {
        {TransactionAction::RequireMainProcessStopped, plan.live_path, {}},
        {TransactionAction::WritePendingJournal, {}, plan.journal_path},
        {TransactionAction::InstallTemporaryHelper,
            "app0:/updater/", "MLVUPD001"},
        {TransactionAction::VerifyStagedPackage, plan.staged_path, {}},
        {TransactionAction::CopyLiveToBackup, plan.live_path, plan.backup_path},
        {TransactionAction::PromoteStagedPackage, plan.staged_path, plan.live_path},
        {TransactionAction::RelaunchMain, plan.live_path, {}},
        {TransactionAction::WaitForLaunchAcknowledgement, plan.journal_path, {}},
        {TransactionAction::DeleteTemporaryHelper, "MLVUPD001", {}},
        {TransactionAction::RemoveBackup, plan.backup_path, {}},
        {TransactionAction::RemovePendingJournal, plan.journal_path, {}},
        {TransactionAction::RemoveStaging, std::string(kStagingRoot), {}}};
    plan.rollback_steps = {
        {TransactionAction::RequireMainProcessStopped, plan.live_path, {}},
        {TransactionAction::RollbackPromoteBackup,
            plan.backup_path, plan.live_path},
        {TransactionAction::RelaunchMain, plan.live_path, {}},
        {TransactionAction::RemovePendingJournal, plan.journal_path, {}},
        {TransactionAction::RemoveStaging, std::string(kStagingRoot), {}}};
    return true;
}

}  // namespace melee::vita::update

extern "C" int melee_update_verify_manifest_c(const char* manifest_text,
    std::size_t manifest_size, const std::uint8_t public_key[32],
    MeleeUpdateSignatureVerifierC verifier, char* error, std::size_t error_size) {
    using namespace melee::vita::update;
    std::string message;
    if (manifest_text == nullptr || public_key == nullptr) {
        message = "Manifest text and public key are required";
        copy_error(error, error_size, message);
        return 0;
    }

    SignedManifest manifest;
    if (!parse_signed_manifest(
            std::string_view(manifest_text, manifest_size), manifest, message))
    {
        copy_error(error, error_size, message);
        return 0;
    }
    std::array<std::uint8_t, 32> key{};
    std::copy_n(public_key, key.size(), key.begin());
    if (!verify_manifest_signature(manifest, key, verifier, message)) {
        copy_error(error, error_size, message);
        return 0;
    }
    copy_error(error, error_size, {});
    return 1;
}
