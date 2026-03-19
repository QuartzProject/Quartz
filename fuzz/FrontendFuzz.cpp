#include "quartz/Quartz.hpp"

#include <cstdint>
#include <string>

// Lexer, parser, sema, lowering and the validator on arbitrary source: diagnostics are fine, crashes are not.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    static const std::string kHost = "fn print(text: str)\n@handle\nstruct Player {\n    id: int\n}\n";
    quartz::SourceInput in[] = {{"host.qzi", kHost},
                                {"plugin.qz", std::string(reinterpret_cast<const char*>(data), size)}};
    quartz::CompileOptions options;
    options.emitBinary = true;
    (void)quartz::compile(in, options);
    return 0;
}
