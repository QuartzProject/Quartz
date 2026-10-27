#include "vm/StateJson.hpp"

#include <charconv>
#include <cmath>
#include <format>

namespace quartz::vm::json {

void Doc::ws() {
    while (pos_ < src_.size() &&
           (src_[pos_] == ' ' || src_[pos_] == '\n' || src_[pos_] == '\r' || src_[pos_] == '\t')) {
        ++pos_;
    }
}

bool Doc::str(std::string& out) {
    ++pos_;
    while (pos_ < src_.size()) {
        char c = src_[pos_++];
        if (c == '"') {
            return true;
        }
        if (c != '\\') {
            out += c;
            continue;
        }
        if (pos_ >= src_.size()) {
            return false;
        }
        char e = src_[pos_++];
        switch (e) {
        case 'n':
            out += '\n';
            break;
        case 't':
            out += '\t';
            break;
        case 'r':
            out += '\r';
            break;
        case 'b':
            out += '\b';
            break;
        case 'f':
            out += '\f';
            break;
        case 'u': {
            std::uint32_t cp = 0;
            if (pos_ + 4 > src_.size() ||
                std::from_chars(src_.data() + pos_, src_.data() + pos_ + 4, cp, 16).ptr != src_.data() + pos_ + 4) {
                return false;
            }
            pos_ += 4;
            if (cp >= 0x80) {
                return false; // exportState only escapes control characters
            }
            out += static_cast<char>(cp);
            break;
        }
        default:
            out += e;
            break;
        }
    }
    return false;
}

bool Doc::parse(std::string_view text) {
    src_ = text;
    pos_ = 0;
    nodes_.clear();
    kids_.clear();
    keys_.clear();
    if (text.size() >= 0xFFFFFFFFU) {
        return false;
    }

    struct Open {
        std::uint32_t node;
        std::size_t pending;
        bool obj;
    };

    std::vector<Open> open;
    std::vector<std::uint32_t> pendingKids;
    std::vector<std::string> pendingKeys;
    std::string key;
    auto readKey = [&] {
        key.clear();
        if (pos_ >= src_.size() || src_[pos_] != '"' || !str(key)) {
            return false;
        }
        ws();
        if (pos_ >= src_.size() || src_[pos_++] != ':') {
            return false;
        }
        ws();
        return true;
    };
    bool wantValue = true;
    ws();
    while (true) {
        if (!wantValue) {
            ws();
            if (open.empty()) {
                return pos_ == src_.size();
            }
            const Open o = open.back();
            if (pos_ < src_.size() && src_[pos_] == ',') {
                ++pos_;
                ws();
                if (o.obj && !readKey()) {
                    return false;
                }
                wantValue = true;
                continue;
            }
            if (pos_ >= src_.size() || src_[pos_] != (o.obj ? '}' : ']')) {
                return false;
            }
            ++pos_;
            Node& n = nodes_[o.node];
            n.first = static_cast<std::uint32_t>(kids_.size());
            n.count = static_cast<std::uint32_t>(pendingKids.size() - o.pending);
            n.end = static_cast<std::uint32_t>(pos_);
            kids_.insert(kids_.end(), pendingKids.begin() + static_cast<std::ptrdiff_t>(o.pending), pendingKids.end());
            for (std::size_t i = o.pending; i < pendingKeys.size(); ++i) {
                keys_.push_back(std::move(pendingKeys[i]));
            }
            pendingKids.resize(o.pending);
            pendingKeys.resize(o.pending);
            open.pop_back();
            continue;
        }
        if (pos_ >= src_.size()) {
            return false;
        }
        auto id = static_cast<std::uint32_t>(nodes_.size());
        nodes_.emplace_back();
        Node& n = nodes_.back();
        n.begin = static_cast<std::uint32_t>(pos_);
        if (!open.empty()) {
            pendingKids.push_back(id);
            pendingKeys.push_back(open.back().obj ? std::move(key) : std::string());
        }
        char c = src_[pos_];
        if (c == '{' || c == '[') {
            bool obj = c == '{';
            n.kind = obj ? Node::Kind::Obj : Node::Kind::Arr;
            ++pos_;
            ws();
            if (pos_ < src_.size() && src_[pos_] == (obj ? '}' : ']')) {
                ++pos_;
                n.first = static_cast<std::uint32_t>(kids_.size());
                n.end = static_cast<std::uint32_t>(pos_);
                wantValue = false;
                continue;
            }
            open.push_back({id, pendingKids.size(), obj});
            if (obj && !readKey()) {
                return false;
            }
            continue;
        }
        wantValue = false;
        if (c == '"') {
            n.kind = Node::Kind::Str;
            if (!str(n.text)) {
                return false;
            }
        } else if (src_.substr(pos_, 4) == "null") {
            pos_ += 4;
        } else if (src_.substr(pos_, 4) == "true" || src_.substr(pos_, 5) == "false") {
            n.kind = Node::Kind::Bool;
            n.b = c == 't';
            pos_ += n.b ? 4 : 5;
        } else {
            std::size_t begin = pos_;
            while (pos_ < src_.size() &&
                   (std::isdigit(static_cast<unsigned char>(src_[pos_])) || src_[pos_] == '-' || src_[pos_] == '+' ||
                    src_[pos_] == '.' || src_[pos_] == 'e' || src_[pos_] == 'E')) {
                ++pos_;
            }
            if (pos_ == begin) {
                return false;
            }
            n.kind = Node::Kind::Num;
            n.text.assign(src_.substr(begin, pos_ - begin));
        }
        nodes_[id].end = static_cast<std::uint32_t>(pos_);
    }
}

const Node* Doc::get(const Node& obj, std::string_view key) const {
    if (obj.kind != Node::Kind::Obj) {
        return nullptr;
    }
    for (std::size_t i = 0; i < obj.count; ++i) {
        if (keys_[obj.first + i] == key) {
            return &child(obj, i);
        }
    }
    return nullptr;
}

void writeString(std::string& out, std::string_view s) {
    out += '"';
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out += std::format("\\u{:04x}", static_cast<unsigned>(c));
        } else {
            out += c;
        }
    }
    out += '"';
}

void writeDouble(std::string& out, double d) {
    if (std::isnan(d)) {
        out += "\"nan\"";
    } else if (std::isinf(d)) {
        out += d > 0 ? "\"inf\"" : "\"-inf\"";
    } else {
        char buf[40];
        auto r = std::to_chars(buf, buf + sizeof buf, d);
        out.append(buf, static_cast<std::size_t>(r.ptr - buf));
    }
}

} // namespace quartz::vm::json
