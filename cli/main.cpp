#include "Fmt.hpp"
#include "quartz/Quartz.hpp"

#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view kStdInterface = R"(fn print(text: str)
@handle
@awaitable
struct Delay {}
impl i64 {
    @property
    fn ticks(self) -> Delay
}
)";

bool hostTicks(quartz::vm::HostCall& call, void*) {
    call.result.u = call.args[0].u;
    return true;
}

bool hostPrint(quartz::vm::HostCall& call, void*) {
    std::string_view s = static_cast<const quartz::vm::Str*>(call.args[0].p)->view();
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fputc('\n', stdout);
    return true;
}

int usage() {
    std::fprintf(stderr, "usage: quartz check|ir|run|dis <file.qz|file.qzi>... | <file.qzb> [--entry name]\n"
                         "                [--trust <public key hex>]... [--require-signature]\n"
                         "       quartz build <file.qz|file.qzi>... -o <out.qzb> [--sign <seed file>] [--strip-lines]\n"
                         "       quartz keygen <seed file>\n"
                         "       quartz fmt [--check] <file|dir>... | quartz fmt -\n");
    return 2;
}

std::optional<std::string> readFile(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot read %s\n", path);
        return std::nullopt;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::span<const std::uint8_t> asBytes(std::string_view s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

bool writeFile(const std::string& path, std::span<const std::uint8_t> bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return false;
    }
    return true;
}

std::string toHex(std::span<const std::uint8_t> bytes) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out;
    for (std::uint8_t b : bytes) {
        out += kDigits[b >> 4];
        out += kDigits[b & 15];
    }
    return out;
}

template <std::size_t N>
std::optional<std::array<std::uint8_t, N>> fromHex(std::string_view text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.remove_suffix(1);
    }
    if (text.size() != N * 2) {
        return std::nullopt;
    }
    auto digit = [](char c) {
        return c >= '0' && c <= '9'   ? c - '0'
               : c >= 'a' && c <= 'f' ? c - 'a' + 10
               : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                      : -1;
    };
    std::array<std::uint8_t, N> out{};
    for (std::size_t i = 0; i < N; ++i) {
        int hi = digit(text[2 * i]);
        int lo = digit(text[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return std::nullopt;
        }
        out[i] = static_cast<std::uint8_t>(hi << 4 | lo);
    }
    return out;
}

int keygen(const char* path) {
    std::random_device rd;
    quartz::ir::SecretSeed seed{};
    for (std::uint8_t& b : seed) {
        b = static_cast<std::uint8_t>(rd());
    }
    if (!writeFile(path, asBytes(toHex(seed) + "\n"))) {
        return 2;
    }
    std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    std::printf("%s\n", toHex(quartz::ir::publicKey(seed)).c_str());
    return 0;
}

int runProgram(const std::shared_ptr<const quartz::vm::Program>& program, const std::string& entry) {
    quartz::vm::Sandbox sb(program);
    sb.bind("print", hostPrint);
    sb.bind("i64.ticks", hostTicks);
    if (auto s = sb.start(); !s) {
        std::fprintf(stderr, "%s\n", s.error().describe().c_str());
        return 3;
    }
    std::uint32_t fn = program->findFunction(entry);
    if (fn == ~0U) {
        std::fprintf(stderr, "no function '%s'\n", entry.c_str());
        return 2;
    }
    auto t = sb.spawn(fn, {});
    std::uint64_t ticks = 0;
    std::uint64_t yields = 0;
    while (t && t->kind != quartz::vm::TaskOutcome::Kind::Done) {
        if (t->kind == quartz::vm::TaskOutcome::Kind::Wait) {
            ticks += t->value.u;
        } else if (t->kind == quartz::vm::TaskOutcome::Kind::Yield) {
            ++yields;
        }
        t = sb.resume(t->task);
    }
    if (!t) {
        std::fprintf(stderr, "%s\n", t.error().describe().c_str());
        return 3;
    }
    if (ticks > 0 || yields > 0) {
        std::fprintf(stderr, "[%llu ticks waited, %llu fuel yields, %zu bytes in use]\n",
                     static_cast<unsigned long long>(ticks), static_cast<unsigned long long>(yields), sb.memoryUsed());
    }
    if (program->functions[fn].result == quartz::ir::kI64) {
        std::printf("%lld\n", static_cast<long long>(t->value.i));
    }
    return 0;
}

struct Args {
    std::vector<quartz::SourceInput> inputs;
    std::string binaryPath;
    std::string entry = "main";
    std::string output;
    std::string seedPath;
    std::vector<quartz::ir::PublicKey> trusted;
    bool requireSignature = false;
    bool lines = true;
};

std::optional<Args> parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 2; i < argc; ++i) {
        std::string_view arg = argv[i];
        bool hasValue = i + 1 < argc;
        if (arg == "--entry" && hasValue) {
            a.entry = argv[++i];
        } else if (arg == "-o" && hasValue) {
            a.output = argv[++i];
        } else if (arg == "--sign" && hasValue) {
            a.seedPath = argv[++i];
        } else if (arg == "--trust" && hasValue) {
            auto key = fromHex<32>(argv[++i]);
            if (!key) {
                std::fprintf(stderr, "--trust needs a 64 digit hex public key\n");
                return std::nullopt;
            }
            a.trusted.push_back(*key);
        } else if (arg == "--require-signature") {
            a.requireSignature = true;
        } else if (arg == "--strip-lines") {
            a.lines = false;
        } else if (arg.ends_with(".qzb")) {
            a.binaryPath = argv[i];
        } else {
            auto text = readFile(argv[i]);
            if (!text) {
                return std::nullopt;
            }
            a.inputs.push_back({argv[i], std::move(*text)});
        }
    }
    return a;
}

int build(Args& a) {
    if (a.output.empty() || a.inputs.empty() || !a.binaryPath.empty()) {
        return usage();
    }
    a.inputs.push_back({"<std>.qzi", std::string(kStdInterface)});
    quartz::CompileOptions opts;
    opts.emitBinary = true;
    opts.binaryLines = a.lines;
    quartz::CompileOutput out = quartz::compile(a.inputs, opts);
    std::fputs(out.diagnostics.c_str(), stderr);
    if (!out.program) {
        return 1;
    }
    if (!a.seedPath.empty()) {
        auto text = readFile(a.seedPath.c_str());
        auto seed = text ? fromHex<32>(*text) : std::nullopt;
        if (!seed) {
            std::fprintf(stderr, "%s does not hold a 64 digit hex seed\n", a.seedPath.c_str());
            return 2;
        }
        if (auto s = quartz::ir::sign(out.binary, *seed); !s) {
            std::fprintf(stderr, "%s\n", s.error().c_str());
            return 1;
        }
    }
    return writeFile(a.output, out.binary) ? 0 : 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        return usage();
    }
    std::string_view cmd = argv[1];
    if (cmd == "fmt") {
        return runFmt(argc, argv);
    }
    if (cmd == "keygen") {
        return keygen(argv[2]);
    }
    auto args = parseArgs(argc, argv);
    if (!args) {
        return 2;
    }
    if (cmd == "build") {
        return build(*args);
    }
    std::shared_ptr<const quartz::vm::Program> program;
    std::string irText;
    if (!args->binaryPath.empty()) {
        if (!args->inputs.empty()) {
            std::fprintf(stderr, "a .qzb module cannot be mixed with source files\n");
            return 2;
        }
        auto bytes = readFile(args->binaryPath.c_str());
        if (!bytes) {
            return 2;
        }
        quartz::ir::ReadOptions ro;
        ro.trustedKeys = args->trusted;
        ro.requireSignature = args->requireSignature;
        auto loaded = quartz::load(asBytes(*bytes), ro);
        if (!loaded) {
            std::fprintf(stderr, "%s: %s\n", args->binaryPath.c_str(), loaded.error().c_str());
            return 1;
        }
        program = *loaded;
        if (cmd == "ir") {
            irText = quartz::ir::print(*program->module);
        }
    } else {
        args->inputs.push_back({"<std>.qzi", std::string(kStdInterface)});
        quartz::CompileOptions opts;
        opts.printIr = cmd == "ir";
        quartz::CompileOutput out = quartz::compile(args->inputs, opts);
        std::fputs(out.diagnostics.c_str(), stderr);
        if (cmd == "ir" && !out.program) {
            std::fputs(out.irText.c_str(), stdout);
        }
        if (!out.program) {
            return 1;
        }
        program = out.program;
        irText = std::move(out.irText);
    }
    if (cmd == "check") {
        return 0;
    }
    if (cmd == "ir") {
        std::fputs(irText.c_str(), stdout);
        return 0;
    }
    if (cmd == "dis") {
        for (std::uint32_t f = 0; f < program->functions.size(); ++f) {
            std::fputs(quartz::vm::disassemble(*program, f).c_str(), stdout);
        }
        return 0;
    }
    if (cmd != "run") {
        return usage();
    }
    return runProgram(program, args->entry);
}
