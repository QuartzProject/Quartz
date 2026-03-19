#include "Commands.hpp"
#include "quartz/Package.hpp"

#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <system_error>

namespace {

bool write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", path.string().c_str());
        return false;
    }
    return true;
}

} // namespace

int newProject(std::string_view name) {
    std::string manifest = std::format("name = \"{}\"\nversion = \"0.1.0\"\nexports = [\"greet\"]\n", name);
    if (auto m = quartz::parseManifest(manifest); !m) {
        std::fprintf(stderr, "%s\n", m.error().c_str());
        return 2;
    }
    std::filesystem::path dir(std::string(name.substr(name.rfind('.') + 1)));
    std::error_code ec;
    if (std::filesystem::exists(dir, ec)) {
        std::fprintf(stderr, "%s already exists\n", dir.string().c_str());
        return 2;
    }
    std::filesystem::create_directories(dir / "src", ec);
    if (ec) {
        std::fprintf(stderr, "cannot create %s: %s\n", dir.string().c_str(), ec.message().c_str());
        return 2;
    }
    std::string source = std::format(R"(package {}

pub fn greet(name: str) -> str {{
    return "Hello, {{name}}!"
}}

@test
fn greetsByName() {{
    if greet("Ada") != "Hello, Ada!" {{
        panic("unexpected greeting")
    }}
}}
)",
                                     name);
    if (!write(dir / "package.toml", manifest) || !write(dir / "src" / "main.qz", source)) {
        return 2;
    }
    std::printf("created %s: quartz test %s, quartz build %s -o %s.qzb\n", dir.string().c_str(), dir.string().c_str(),
                dir.string().c_str(), dir.string().c_str());
    return 0;
}
