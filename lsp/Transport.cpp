#include "Transport.hpp"

#include <charconv>
#include <cstdio>
#include <string_view>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <cerrno>
#include <poll.h>
#include <unistd.h>
#endif

namespace quartz::lsp {

namespace {

constexpr std::size_t kMaxMessage = 256u << 20;

struct Frame {
    std::size_t headerEnd = 0; // offset of the body
    std::size_t length = 0;
    bool complete = false;
    bool malformed = false;
};

Frame frameOf(const std::string& buffer) {
    Frame f;
    std::size_t end = buffer.find("\r\n\r\n");
    if (end == std::string::npos) {
        return f;
    }
    f.headerEnd = end + 4;
    std::string_view headers(buffer.data(), end);
    bool found = false;
    while (!headers.empty()) {
        std::size_t nl = headers.find("\r\n");
        std::string_view line = headers.substr(0, nl);
        headers = nl == std::string_view::npos ? std::string_view{} : headers.substr(nl + 2);
        constexpr std::string_view kKey = "content-length:";
        if (line.size() < kKey.size()) {
            continue;
        }
        bool match = true;
        for (std::size_t i = 0; i < kKey.size(); ++i) {
            char c = line[i];
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
            match = match && c == kKey[i];
        }
        if (!match) {
            continue;
        }
        std::string_view v = line.substr(kKey.size());
        while (!v.empty() && v.front() == ' ') {
            v.remove_prefix(1);
        }
        auto r = std::from_chars(v.data(), v.data() + v.size(), f.length);
        found = r.ec == std::errc{};
    }
    f.malformed = !found || f.length > kMaxMessage;
    f.complete = !f.malformed && buffer.size() >= f.headerEnd + f.length;
    return f;
}

} // namespace

Transport::Transport() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

bool Transport::haveMessage() const {
    Frame f = frameOf(buffer_);
    return f.complete || f.malformed;
}

bool Transport::fill(int millis) {
    if (eof_) {
        return false;
    }
    char chunk[65536];
#ifdef _WIN32
    if (millis >= 0) {
        return false;
    }
    int n = _read(0, chunk, sizeof chunk);
#else
    if (millis >= 0) {
        pollfd p{0, POLLIN, 0};
        int r = ::poll(&p, 1, millis);
        if (r <= 0) {
            return false;
        }
    }
    ssize_t n = 0;
    do {
        n = ::read(0, chunk, sizeof chunk);
    } while (n < 0 && errno == EINTR);
#endif
    if (n <= 0) {
        eof_ = true;
        return false;
    }
    buffer_.append(chunk, static_cast<std::size_t>(n));
    return true;
}

bool Transport::pending(int millis) {
    if (haveMessage()) {
        return true;
    }
    while (fill(millis)) {
        if (haveMessage()) {
            return true;
        }
        millis = 0;
    }
    return haveMessage() || eof_;
}

std::optional<std::string> Transport::read() {
    while (true) {
        Frame f = frameOf(buffer_);
        if (f.malformed) {
            if (f.length > kMaxMessage) {
                return std::nullopt;
            }
            buffer_.erase(0, f.headerEnd);
            continue;
        }
        if (f.complete) {
            std::string body = buffer_.substr(f.headerEnd, f.length);
            buffer_.erase(0, f.headerEnd + f.length);
            return body;
        }
        if (!fill(-1)) {
            return std::nullopt;
        }
    }
}

void Transport::write(const std::string& body) {
    std::string header = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    std::fwrite(header.data(), 1, header.size(), stdout);
    std::fwrite(body.data(), 1, body.size(), stdout);
    std::fflush(stdout);
}

} // namespace quartz::lsp
