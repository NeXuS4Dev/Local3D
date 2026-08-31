#pragma once
/// @file Json.hpp
/// @brief A compact JSON value model, parser and writer.
///
/// Used for human editable files: scene/prefab text, material parameters, the
/// asset database index and editor settings.  Binary formats (BinaryStream.hpp)
/// are used for cooked runtime data.
///
/// The parser is intentionally strict about structure but forgiving about
/// whitespace; it reports the first error with a line number instead of
/// throwing.

#include "local3d/core/Common.hpp"
#include "local3d/core/Result.hpp"

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace l3d::serial {

class JsonValue;
using JsonArray = std::vector<JsonValue>;
/// Ordered map keeps output stable across runs, which matters for diffs and for
/// deterministic cook output.
using JsonObject = std::map<std::string, JsonValue, std::less<>>;

/// A JSON node.  Value semantics; children are held by value (documents are
/// small and short lived compared to a frame).
class JsonValue {
public:
    enum class Type : u8 { Null, Bool, Number, String, Array, Object };

    JsonValue() noexcept = default;
    JsonValue(std::nullptr_t) noexcept {} // NOLINT(google-explicit-constructor)
    JsonValue(bool value) noexcept : storage_(value) {} // NOLINT
    JsonValue(f64 value) noexcept : storage_(value) {}   // NOLINT
    JsonValue(i64 value) noexcept : storage_(static_cast<f64>(value)) {} // NOLINT
    JsonValue(i32 value) noexcept : storage_(static_cast<f64>(value)) {} // NOLINT
    JsonValue(u32 value) noexcept : storage_(static_cast<f64>(value)) {} // NOLINT
    JsonValue(std::string value) noexcept : storage_(std::move(value)) {} // NOLINT
    JsonValue(const char* value) : storage_(std::string(value)) {}        // NOLINT
    JsonValue(JsonArray value) noexcept : storage_(std::move(value)) {}    // NOLINT
    JsonValue(JsonObject value) noexcept : storage_(std::move(value)) {}   // NOLINT

    [[nodiscard]] static JsonValue MakeArray() { return JsonValue(JsonArray{}); }
    [[nodiscard]] static JsonValue MakeObject() { return JsonValue(JsonObject{}); }

    [[nodiscard]] Type GetType() const noexcept {
        switch (storage_.index()) {
            case 0: return Type::Null;
            case 1: return Type::Bool;
            case 2: return Type::Number;
            case 3: return Type::String;
            case 4: return Type::Array;
            default: return Type::Object;
        }
    }

    [[nodiscard]] bool IsNull() const noexcept { return GetType() == Type::Null; }
    [[nodiscard]] bool IsNumber() const noexcept { return GetType() == Type::Number; }
    [[nodiscard]] bool IsString() const noexcept { return GetType() == Type::String; }
    [[nodiscard]] bool IsArray() const noexcept { return GetType() == Type::Array; }
    [[nodiscard]] bool IsObject() const noexcept { return GetType() == Type::Object; }

    [[nodiscard]] bool AsBool(bool fallback = false) const noexcept {
        return storage_.index() == 1 ? std::get<1>(storage_) : fallback;
    }
    [[nodiscard]] f64 AsNumber(f64 fallback = 0.0) const noexcept {
        return storage_.index() == 2 ? std::get<2>(storage_) : fallback;
    }
    [[nodiscard]] i64 AsInt(i64 fallback = 0) const noexcept {
        return storage_.index() == 2 ? static_cast<i64>(std::get<2>(storage_)) : fallback;
    }
    [[nodiscard]] std::string_view AsString(std::string_view fallback = {}) const noexcept {
        return storage_.index() == 3 ? std::string_view(std::get<3>(storage_)) : fallback;
    }

    /// Array access.  Returns a static null value when out of range so callers
    /// can chain without checks.
    [[nodiscard]] const JsonValue& operator[](usize index) const;
    [[nodiscard]] usize Size() const noexcept;
    [[nodiscard]] const JsonArray& AsArray() const;
    [[nodiscard]] JsonArray& AsArray();

    /// Object access.
    [[nodiscard]] const JsonValue& operator[](std::string_view key) const;
    [[nodiscard]] bool Contains(std::string_view key) const;
    [[nodiscard]] const JsonObject& AsObject() const;
    [[nodiscard]] JsonObject& AsObject();

    /// Mutators used while building documents.
    void Set(std::string_view key, JsonValue value);
    void Push(JsonValue value);

    /// Pretty printed text.  `indent` spaces per level.
    [[nodiscard]] std::string Dump(u32 indent = 2) const;

    /// Parse a document.  On failure the returned status carries the line.
    [[nodiscard]] static Result<JsonValue> Parse(std::string_view text);

private:
    void DumpTo(std::string& out, u32 indent, u32 depth) const;

    std::variant<std::monostate, bool, f64, std::string, JsonArray, JsonObject> storage_;
};

/// Escape a string for JSON output.
[[nodiscard]] std::string JsonEscape(std::string_view text);

} // namespace l3d::serial
