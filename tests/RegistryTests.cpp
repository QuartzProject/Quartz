#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "quartz/Registry.hpp"

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
namespace reg = quartz::registry;

namespace {

void write(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

std::string read(const fs::path& path) {
    std::stringstream ss;
    ss << std::ifstream(path, std::ios::binary).rdbuf();
    return ss.str();
}

int cli(const fs::path& dir, const std::string& args) {
    std::string cmd = std::format("cd \"{}\" && \"{}\" {} > out.txt 2> err.txt", dir.string(), QUARTZ_CLI_PATH, args);
    int status = std::system(cmd.c_str());
    if (status != 0) {
        MESSAGE("quartz " << args << " failed:\n" << read(dir / "err.txt"));
    }
    return status;
}

void library(const fs::path& dir, std::string_view name, std::string_view version, std::string_view deps,
             std::string_view source) {
    write(dir / "package.toml", std::format("name = \"{}\"\nversion = \"{}\"\n{}", name, version, deps));
    write(dir / "src" / "lib.qz", source);
}

reg::Release release(std::string name, quartz::Version v, std::vector<reg::Dependency> deps,
                     const quartz::ir::SecretSeed& seed) {
    reg::Release r;
    r.name = std::move(name);
    r.version = v;
    r.dependencies = std::move(deps);
    r.checksum.fill(static_cast<std::uint8_t>(v.minor));
    reg::signRelease(r, seed);
    return r;
}

} // namespace

TEST_CASE("libraries and a plugin published to a registry resolve, build and run") {
    fs::path root = fs::path(QUARTZ_TEST_DIR) / "registry-e2e";
    fs::remove_all(root);
    fs::create_directories(root / "reg");
    fs::create_directories(root / "cache");
#ifdef _WIN32
    _putenv_s("QUARTZ_CACHE", (root / "cache").string().c_str());
#else
    setenv("QUARTZ_CACHE", (root / "cache").c_str(), 1);
#endif
    REQUIRE(cli(root, "keygen pub.seed") == 0);
    std::string key = read(root / "out.txt");
    key.resize(64);
    REQUIRE(cli(root, "owner org.example " + key + " --registry reg") == 0);

    for (std::string_view v : {"1.0.0", "1.2.0", "2.0.0"}) {
        library(
            root / "mathx", "org.example.mathx", v, "",
            std::format("package org.example.mathx\n\npub fn pair<T>(a: T, b: T) -> [T] {{\n    return [a, b]\n}}\n\n"
                        "pub fn version() -> str {{\n    return \"{}\"\n}}\n",
                        v));
        REQUIRE(cli(root, "publish mathx --registry reg --sign pub.seed") == 0);
    }
    library(root / "text", "org.example.text", "0.3.1", "\n[dependencies]\n\"org.example.mathx\" = \">=1.1, <2\"\n",
            "package org.example.text\n\nimport org.example.mathx\n\npub fn banner(s: str) -> str {\n"
            "    return \"[{s} x{mathx.pair(s, s).len()} {mathx.version()}]\"\n}\n");
    REQUIRE(cli(root, "publish text --registry reg --sign pub.seed") == 0);
    write(root / "bank" / "package.toml",
          "name = \"org.example.bank\"\nversion = \"2.1.0\"\nkind = \"plugin\"\nexports = [\"deposit\"]\n");
    write(root / "bank" / "src" / "bank.qz", "package org.example.bank\n\nstate Vault {\n    total: int\n}\n\n"
                                             "fn deposit(v: Vault, who: str, n: int) -> int {\n"
                                             "    v.total = v.total + n + who.len()\n    return v.total\n}\n");
    REQUIRE(cli(root, "publish bank --registry reg --sign pub.seed") == 0);

    write(root / "app" / "package.toml", "name = \"org.example.app\"\nversion = \"0.1.0\"\nregistry = \"../reg\"\n");
    write(root / "app" / "src" / "main.qz",
          "import org.example.text\nimport org.example.bank\n\nfn main() -> int {\n    print(text.banner(\"hi\"))\n"
          "    bank.deposit(\"ana\", 10)\n    return bank.deposit(\"bob\", 5)\n}\n");
    REQUIRE(cli(root, "add org.example.text@^0.3 app") == 0);
    REQUIRE(cli(root, "add \"org.example.bank@>=2.0, <3\" app") == 0);
    REQUIRE(cli(root, "run app") == 0);
    CHECK(read(root / "out.txt") == "[hi x2 1.2.0]\n21\n");
    auto lock = reg::parseLock(read(root / "app" / "quartz.lock"));
    REQUIRE_MESSAGE(lock.has_value(), (lock ? "" : lock.error()));
    REQUIRE(lock->find("org.example.mathx") != nullptr);
    CHECK(lock->find("org.example.mathx")->version == quartz::Version{1, 2, 0});
}

TEST_CASE("ranges no published version satisfies are a conflict naming who asks") {
    quartz::ir::SecretSeed seed{};
    seed.fill(3);
    std::vector<reg::Release> mathx = {release("org.example.mathx", {1, 4, 0}, {}, seed),
                                       release("org.example.mathx", {2, 0, 0}, {}, seed)};
    std::vector<reg::Release> text = {release("org.example.text", {0, 3, 0}, {{"org.example.mathx", "^1.2"}}, seed)};
    auto index = [&](const std::string& name) -> std::expected<std::vector<reg::Release>, std::string> {
        return name == "org.example.mathx" ? mathx : text;
    };
    auto root = quartz::parseManifest("name = \"org.example.app\"\nversion = \"0.1.0\"\n[dependencies]\n"
                                      "\"org.example.text\" = \"*\"\n\"org.example.mathx\" = \"^2\"\n");
    REQUIRE(root.has_value());
    auto lock = reg::resolve(*root, index);
    REQUIRE_FALSE(lock.has_value());
    CHECK(lock.error().find("conflict") != std::string::npos);
    CHECK(lock.error().find("^1.2 (from org.example.text 0.3.0)") != std::string::npos);

    SUBCASE("an index entry whose signature does not verify is refused") {
        mathx[1].checksum[0] ^= 1;
        auto tampered = reg::resolve(*root, index);
        REQUIRE_FALSE(tampered.has_value());
        CHECK(tampered.error().find("invalid signature") != std::string::npos);
    }
}
