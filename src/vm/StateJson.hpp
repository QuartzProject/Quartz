#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace quartz::vm::json {

/// One value of a parsed document; containers list their children in the document's flat arrays.
struct Node {
    enum class Kind : std::uint8_t { Null, Bool, Num, Str, Arr, Obj } kind = Kind::Null;
    bool b = false;
    std::uint32_t first = 0; // children are kids[first, first + count)
    std::uint32_t count = 0;
    std::uint32_t begin = 0; // the value's text in the source
    std::uint32_t end = 0;
    std::string text; // strings, and numbers as written so 64-bit integers stay exact
};

/// State documents come back from disk, so the reader is defensive and iterative: any depth fits in memory, not stack.
class Doc {
public:
    bool parse(std::string_view text);

    [[nodiscard]] const Node& root() const { return nodes_[0]; }

    [[nodiscard]] const Node& child(const Node& n, std::size_t i) const { return nodes_[kids_[n.first + i]]; }

    [[nodiscard]] const std::string& key(const Node& n, std::size_t i) const { return keys_[n.first + i]; }

    [[nodiscard]] const Node* get(const Node& obj, std::string_view key) const;

    [[nodiscard]] std::string_view source(const Node& n) const { return src_.substr(n.begin, n.end - n.begin); }

private:
    bool str(std::string& out);
    void ws();

    std::string_view src_;
    std::size_t pos_ = 0;
    std::vector<Node> nodes_;
    std::vector<std::uint32_t> kids_;
    std::vector<std::string> keys_; // beside kids_: the key of each object member, empty for array items
};

void writeString(std::string& out, std::string_view s);
void writeDouble(std::string& out, double d);

} // namespace quartz::vm::json
