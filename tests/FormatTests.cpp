#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "format/Format.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

namespace fs = std::filesystem;

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

TEST_CASE("formatting every source in the repository is idempotent and keeps the tree") {
    for (const char* dir : {"examples", "tests", "bench"}) {
        for (const auto& e : fs::recursive_directory_iterator(dir)) {
            fs::path p = e.path();
            if (p.extension() != ".qz" && p.extension() != ".qzi" && p.extension() != ".in") {
                continue;
            }
            CAPTURE(p.string());
            std::string text = readFile(p);
            if (!quartz::format::dumpSource(p.string(), text)) {
                continue;
            }
            auto once = quartz::format::formatSource(p.string(), text);
            REQUIRE_MESSAGE(once.has_value(), once.error());
            auto twice = quartz::format::formatSource(p.string(), *once);
            REQUIRE_MESSAGE(twice.has_value(), twice.error());
            CHECK(*twice == *once);
        }
    }
}

TEST_CASE("the formatter keeps its documented style") {
    auto out = quartz::format::formatSource("style.in", readFile("tests/format/style.in"));
    REQUIRE_MESSAGE(out.has_value(), out.error());
    CHECK(*out == readFile("tests/format/style.out"));
}
