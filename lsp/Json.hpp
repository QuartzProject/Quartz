#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace quartz::lsp {

/// A JSON value; objects keep insertion order and lookups are linear (LSP messages are small).
class Json {
public:
    enum class Kind : std::uint8_t { Null, Bool, Int, Float, String, Array, Object };

    Json() = default;

    Json(std::nullptr_t) {}

    Json(bool b) : kind_(Kind::Bool), int_(b ? 1 : 0) {}

    Json(int i) : kind_(Kind::Int), int_(i) {}

    Json(std::int64_t i) : kind_(Kind::Int), int_(i) {}

    Json(std::uint32_t i) : kind_(Kind::Int), int_(i) {}

    Json(double d) : kind_(Kind::Float), float_(d) {}

    Json(std::string s) : kind_(Kind::String), str_(std::move(s)) {}

    Json(std::string_view s) : kind_(Kind::String), str_(s) {}

    Json(const char* s) : kind_(Kind::String), str_(s) {}

    static Json array() { return withKind(Kind::Array); }

    static Json object() { return withKind(Kind::Object); }

    [[nodiscard]] Kind kind() const { return kind_; }

    [[nodiscard]] bool isNull() const { return kind_ == Kind::Null; }

    [[nodiscard]] bool isString() const { return kind_ == Kind::String; }

    [[nodiscard]] bool isNumber() const { return kind_ == Kind::Int || kind_ == Kind::Float; }

    [[nodiscard]] bool isObject() const { return kind_ == Kind::Object; }

    [[nodiscard]] bool isArray() const { return kind_ == Kind::Array; }

    /// Member lookup; a shared null when missing or when this is not an object.
    const Json& operator[](std::string_view key) const;
    const Json& operator[](std::size_t index) const;
    [[nodiscard]] bool has(std::string_view key) const;

    [[nodiscard]] std::string_view str() const { return kind_ == Kind::String ? std::string_view(str_) : ""; }

    [[nodiscard]] std::int64_t integer(std::int64_t fallback = 0) const;

    [[nodiscard]] bool boolean(bool fallback = false) const { return kind_ == Kind::Bool ? int_ != 0 : fallback; }

    [[nodiscard]] std::size_t size() const { return kind_ == Kind::Array ? items_.size() : members_.size(); }

    [[nodiscard]] const std::vector<Json>& items() const { return items_; }

    Json& set(std::string key, Json value);
    Json& push(Json value);

    [[nodiscard]] std::string dump() const;
    void dumpTo(std::string& out) const;

    /// Strict RFC 8259 parse with a nesting limit; nullopt on any error.
    static std::optional<Json> parse(std::string_view text);

private:
    static Json withKind(Kind k) {
        Json j;
        j.kind_ = k;
        return j;
    }

    friend class JsonParser;
    Kind kind_ = Kind::Null;
    std::int64_t int_ = 0;
    double float_ = 0;
    std::string str_;
    std::vector<Json> items_;
    std::vector<std::pair<std::string, Json>> members_;
};

void appendJsonString(std::string& out, std::string_view text);

} // namespace quartz::lsp
