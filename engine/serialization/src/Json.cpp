#include "local3d/serialization/Json.hpp"

#include "local3d/core/Format.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>

namespace l3d::serial {
namespace {

/// Recursive descent parser.  Kept iterative where it is easy (arrays/objects
/// recurse, which is fine for engine documents that are never deeply nested).
class JsonParser {
public:
    explicit JsonParser(std::string_view text) noexcept : text_(text) {}

    [[nodiscard]] Result<JsonValue> Parse() {
        SkipWhitespace();
        JsonValue value;
        if (!ParseValue(value)) {
            return Unexpected(Status{StatusCode::ParseError, fmt::Format("JSON error at line {}", line_)});
        }
        SkipWhitespace();
        if (position_ != text_.size()) {
            return Unexpected(
                Status{StatusCode::ParseError, fmt::Format("Trailing characters at line {}", line_)});
        }
        return value;
    }

private:
    [[nodiscard]] bool AtEnd() const noexcept { return position_ >= text_.size(); }
    [[nodiscard]] char Peek() const noexcept { return AtEnd() ? '\0' : text_[position_]; }

    void Advance() noexcept {
        if (AtEnd()) {
            return;
        }
        if (text_[position_] == '\n') {
            ++line_;
        }
        ++position_;
    }

    void SkipWhitespace() noexcept {
        while (!AtEnd()) {
            const char c = text_[position_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                Advance();
            } else {
                break;
            }
        }
    }

    [[nodiscard]] bool Consume(std::string_view literal) noexcept {
        if (text_.substr(position_, literal.size()) != literal) {
            return false;
        }
        for (usize i = 0; i < literal.size(); ++i) {
            Advance();
        }
        return true;
    }

    bool ParseValue(JsonValue& out) {
        SkipWhitespace();
        switch (Peek()) {
            case '{': return ParseObject(out);
            case '[': return ParseArray(out);
            case '"': {
                std::string text;
                if (!ParseString(text)) {
                    return false;
                }
                out = JsonValue(std::move(text));
                return true;
            }
            case 't':
                if (!Consume("true")) {
                    return false;
                }
                out = JsonValue(true);
                return true;
            case 'f':
                if (!Consume("false")) {
                    return false;
                }
                out = JsonValue(false);
                return true;
            case 'n':
                if (!Consume("null")) {
                    return false;
                }
                out = JsonValue(nullptr);
                return true;
            default:
                return ParseNumber(out);
        }
    }

    bool ParseNumber(JsonValue& out) {
        const usize start = position_;
        if (Peek() == '-' || Peek() == '+') {
            Advance();
        }
        while (!AtEnd() && ((text_[position_] >= '0' && text_[position_] <= '9') ||
                            text_[position_] == '.' || text_[position_] == 'e' ||
                            text_[position_] == 'E' || text_[position_] == '-' ||
                            text_[position_] == '+')) {
            Advance();
        }
        if (position_ == start) {
            return false;
        }
        const std::string_view literal = text_.substr(start, position_ - start);
        f64 value = 0.0;
        const auto result = std::from_chars(literal.data(), literal.data() + literal.size(), value);
        if (result.ec != std::errc{}) {
            return false;
        }
        out = JsonValue(value);
        return true;
    }

    bool ParseString(std::string& out) {
        if (Peek() != '"') {
            return false;
        }
        Advance();
        out.clear();
        while (!AtEnd()) {
            const char c = text_[position_];
            if (c == '"') {
                Advance();
                return true;
            }
            if (c == '\\') {
                Advance();
                if (AtEnd()) {
                    return false;
                }
                const char escaped = text_[position_];
                Advance();
                switch (escaped) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        u32 codePoint = 0;
                        if (!ParseHex4(codePoint)) {
                            return false;
                        }
                        AppendUtf8(out, codePoint);
                        break;
                    }
                    default:
                        return false;
                }
                continue;
            }
            out.push_back(c);
            Advance();
        }
        return false; // Unterminated string.
    }

    bool ParseHex4(u32& out) {
        if (position_ + 4 > text_.size()) {
            return false;
        }
        u32 value = 0;
        for (usize i = 0; i < 4; ++i) {
            const char c = text_[position_];
            u32 digit = 0;
            if (c >= '0' && c <= '9') {
                digit = static_cast<u32>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                digit = static_cast<u32>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                digit = static_cast<u32>(c - 'A' + 10);
            } else {
                return false;
            }
            value = (value << 4) | digit;
            Advance();
        }
        out = value;
        return true;
    }

    static void AppendUtf8(std::string& out, u32 codePoint) {
        if (codePoint < 0x80) {
            out.push_back(static_cast<char>(codePoint));
        } else if (codePoint < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
            out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
    }

    bool ParseArray(JsonValue& out) {
        Advance(); // '['
        JsonArray items;
        SkipWhitespace();
        if (Peek() == ']') {
            Advance();
            out = JsonValue(std::move(items));
            return true;
        }
        for (;;) {
            JsonValue item;
            if (!ParseValue(item)) {
                return false;
            }
            items.push_back(std::move(item));
            SkipWhitespace();
            if (Peek() == ',') {
                Advance();
                continue;
            }
            if (Peek() == ']') {
                Advance();
                out = JsonValue(std::move(items));
                return true;
            }
            return false;
        }
    }

    bool ParseObject(JsonValue& out) {
        Advance(); // '{'
        JsonObject members;
        SkipWhitespace();
        if (Peek() == '}') {
            Advance();
            out = JsonValue(std::move(members));
            return true;
        }
        for (;;) {
            SkipWhitespace();
            std::string key;
            if (!ParseString(key)) {
                return false;
            }
            SkipWhitespace();
            if (Peek() != ':') {
                return false;
            }
            Advance();
            JsonValue value;
            if (!ParseValue(value)) {
                return false;
            }
            members.emplace(std::move(key), std::move(value));
            SkipWhitespace();
            if (Peek() == ',') {
                Advance();
                continue;
            }
            if (Peek() == '}') {
                Advance();
                out = JsonValue(std::move(members));
                return true;
            }
            return false;
        }
    }

    std::string_view text_;
    usize position_ = 0;
    u32 line_ = 1;
};

} // namespace

const JsonValue& JsonValue::operator[](usize index) const {
    static const JsonValue kNull{};
    if (storage_.index() != 4) {
        return kNull;
    }
    const JsonArray& items = std::get<4>(storage_);
    return index < items.size() ? items[index] : kNull;
}

usize JsonValue::Size() const noexcept {
    if (storage_.index() == 4) {
        return std::get<4>(storage_).size();
    }
    if (storage_.index() == 5) {
        return std::get<5>(storage_).size();
    }
    return 0;
}

const JsonArray& JsonValue::AsArray() const {
    static const JsonArray kEmpty{};
    return storage_.index() == 4 ? std::get<4>(storage_) : kEmpty;
}

JsonArray& JsonValue::AsArray() {
    if (storage_.index() != 4) {
        storage_ = JsonArray{};
    }
    return std::get<4>(storage_);
}

const JsonValue& JsonValue::operator[](std::string_view key) const {
    static const JsonValue kNull{};
    if (storage_.index() != 5) {
        return kNull;
    }
    const JsonObject& members = std::get<5>(storage_);
    const auto found = members.find(key);
    return found != members.end() ? found->second : kNull;
}

bool JsonValue::Contains(std::string_view key) const {
    return storage_.index() == 5 && std::get<5>(storage_).contains(key);
}

const JsonObject& JsonValue::AsObject() const {
    static const JsonObject kEmpty{};
    return storage_.index() == 5 ? std::get<5>(storage_) : kEmpty;
}

JsonObject& JsonValue::AsObject() {
    if (storage_.index() != 5) {
        storage_ = JsonObject{};
    }
    return std::get<5>(storage_);
}

void JsonValue::Set(std::string_view key, JsonValue value) {
    AsObject().insert_or_assign(std::string(key), std::move(value));
}

void JsonValue::Push(JsonValue value) { AsArray().push_back(std::move(value)); }

std::string JsonEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[8]{};
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c));
                    out += buffer;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

void JsonValue::DumpTo(std::string& out, u32 indent, u32 depth) const {
    auto writeIndent = [&out, indent, depth](u32 extra) {
        if (indent == 0) {
            return;
        }
        out.push_back('\n');
        out.append(static_cast<usize>((depth + extra) * indent), ' ');
    };

    switch (GetType()) {
        case Type::Null:
            out += "null";
            break;
        case Type::Bool:
            out += AsBool() ? "true" : "false";
            break;
        case Type::Number: {
            char buffer[48]{};
            const f64 value = AsNumber();
            if (std::isfinite(value) && value == std::floor(value) && std::fabs(value) < 1e15) {
                std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
            } else {
                std::snprintf(buffer, sizeof(buffer), "%.6g", value);
            }
            out += buffer;
            break;
        }
        case Type::String:
            out += '"';
            out += JsonEscape(AsString());
            out += '"';
            break;
        case Type::Array: {
            const JsonArray& items = AsArray();
            if (items.empty()) {
                out += "[]";
                break;
            }
            out += '[';
            for (usize i = 0; i < items.size(); ++i) {
                if (i > 0) {
                    out += ',';
                }
                writeIndent(1);
                items[i].DumpTo(out, indent, depth + 1);
            }
            writeIndent(0);
            out += ']';
            break;
        }
        case Type::Object: {
            const JsonObject& members = AsObject();
            if (members.empty()) {
                out += "{}";
                break;
            }
            out += '{';
            bool first = true;
            for (const auto& entry : members) {
                if (!first) {
                    out += ',';
                }
                first = false;
                writeIndent(1);
                out += '"';
                out += JsonEscape(entry.first);
                out += "\":";
                if (indent > 0) {
                    out += ' ';
                }
                entry.second.DumpTo(out, indent, depth + 1);
            }
            writeIndent(0);
            out += '}';
            break;
        }
    }
}

std::string JsonValue::Dump(u32 indent) const {
    std::string out;
    DumpTo(out, indent, 0);
    return out;
}

Result<JsonValue> JsonValue::Parse(std::string_view text) {
    JsonParser parser(text);
    return parser.Parse();
}

} // namespace l3d::serial
