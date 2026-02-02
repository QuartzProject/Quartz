#pragma once

#include <optional>
#include <string>

namespace quartz::lsp {

/// Content-Length framed messages over stdin and stdout.
class Transport {
public:
    Transport();

    /// Next message body; nullopt at end of input or on a broken stream.
    std::optional<std::string> read();
    /// True when a whole message is already available or arrives within `millis`.
    bool pending(int millis);
    void write(const std::string& body);

private:
    bool fill(int millis);
    [[nodiscard]] bool haveMessage() const;

    std::string buffer_;
    bool eof_ = false;
};

} // namespace quartz::lsp
