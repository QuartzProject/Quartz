#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "lsp/Json.hpp"
#include "quartz/Quartz.hpp"
#include "std/Std.hpp"
#include "vm/Profiler.hpp"

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using quartz::lsp::Json;

constexpr std::string_view kProgram = R"(struct Point {
    x: int
    y: int
}

fn divide(a: int, b: int) -> int {
    let q = a / b
    return q
}

fn main() {
    let p = Point(3, 4)

    let xs = [10, 20, 30]
    var total = 0
    for i in 0..4 {
        total += xs.len() + i
    }
    print("{total}")
    print("{divide(total, p.x - 3)}")
}
)";

int seq = 0;

std::string request(std::string_view command, Json args) {
    std::string body = Json::object()
                           .set("seq", ++seq)
                           .set("type", "request")
                           .set("command", command)
                           .set("arguments", std::move(args))
                           .dump();
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

std::vector<Json> readFrames(const std::string& out) {
    std::vector<Json> msgs;
    std::size_t pos = 0;
    while (true) {
        std::size_t header = out.find("Content-Length: ", pos);
        std::size_t body = out.find("\r\n\r\n", pos);
        if (header == std::string::npos || body == std::string::npos) {
            break;
        }
        std::size_t length = std::stoul(out.substr(header + 16, body - header - 16));
        if (auto j = Json::parse(std::string_view(out).substr(body + 4, length))) {
            msgs.push_back(std::move(*j));
        }
        pos = body + 4 + length;
    }
    return msgs;
}

const Json& response(const std::vector<Json>& msgs, std::int64_t requestSeq) {
    static const Json kMissing;
    for (const Json& m : msgs) {
        if (m["type"].str() == "response" && m["request_seq"].integer() == requestSeq) {
            return m;
        }
    }
    return kMissing;
}

std::vector<const Json*> events(const std::vector<Json>& msgs, std::string_view name) {
    std::vector<const Json*> out;
    for (const Json& m : msgs) {
        if (m["type"].str() == "event" && m["event"].str() == name) {
            out.push_back(&m);
        }
    }
    return out;
}

std::string variablesText(const Json& r) {
    std::string out;
    for (const Json& v : r["body"]["variables"].items()) {
        out += std::string(v["name"].str()) + "=" + std::string(v["value"].str()) + ":" + std::string(v["type"].str()) +
               ";";
    }
    return out;
}

} // namespace

TEST_CASE("scripted debug session: breakpoints, steps, variables and a trap") {
    fs::path dir = fs::path(QUARTZ_TEST_DIR) / "dap-session";
    fs::create_directories(dir);
    fs::path source = fs::absolute(dir / "main.qz");
    std::ofstream(source, std::ios::binary) << kProgram;
    Json bps = Json::array();
    bps.push(Json::object().set("line", 13));
    bps.push(Json::object().set("line", 17).set("condition", "i == 2"));
    bps.push(Json::object().set("line", 99));
    Json path = Json::object().set("path", source.string());
    // Inspection requests wait for the next stop, so one scripted input drives the whole session.
    std::string in;
    in += request("initialize", Json::object().set("adapterID", "quartz"));
    in += request("launch", Json::object().set("program", source.string()));
    in += request("setBreakpoints", Json::object().set("source", path).set("breakpoints", bps));
    in += request("configurationDone", Json::object());
    in += request("stackTrace", Json::object().set("threadId", 1));
    in += request("scopes", Json::object().set("frameId", 1));
    in += request("variables", Json::object().set("variablesReference", 2));
    in += request("next", Json::object().set("threadId", 1));
    in += request("stackTrace", Json::object().set("threadId", 1));
    in += request("continue", Json::object().set("threadId", 1));
    in += request("evaluate", Json::object().set("expression", "total").set("frameId", 1));
    in += request("setBreakpoints", Json::object().set("source", path).set("breakpoints", Json::array()));
    in += request("continue", Json::object().set("threadId", 1));
    in += request("stackTrace", Json::object().set("threadId", 1));
    in += request("scopes", Json::object().set("frameId", 1));
    in += request("variables", Json::object().set("variablesReference", 1));
    in += request("exceptionInfo", Json::object().set("threadId", 1));
    in += request("evaluate", Json::object().set("expression", "p.y").set("frameId", 2));
    std::ofstream(dir / "in", std::ios::binary) << in;
    std::string cmd = std::string("\"") + QUARTZ_CLI_PATH + "\" debug --dap < \"" + (dir / "in").string() + "\" > \"" +
                      (dir / "out").string() + "\" 2> \"" + (dir / "err").string() + "\"";
    REQUIRE(std::system(cmd.c_str()) == 0);
    std::stringstream ss;
    ss << std::ifstream(dir / "out", std::ios::binary).rdbuf();
    std::vector<Json> msgs = readFrames(ss.str());

    CHECK(response(msgs, 1)["body"]["supportsConditionalBreakpoints"].boolean());
    CHECK(response(msgs, 2)["success"].boolean());
    const Json& set = response(msgs, 3)["body"]["breakpoints"];
    REQUIRE(set.size() == 3);
    CHECK(set[0]["line"].integer() == 14); // line 13 is blank: the breakpoint moves to the next line with code
    CHECK(set[1]["verified"].boolean());
    CHECK_FALSE(set[2]["verified"].boolean());

    const Json& top = response(msgs, 5)["body"]["stackFrames"][0];
    CHECK(top["name"].str() == "main");
    CHECK(top["line"].integer() == 14);
    CHECK(variablesText(response(msgs, 7)) == "p=Point { x: 3, y: 4 }:Point;");
    CHECK(response(msgs, 9)["body"]["stackFrames"][0]["line"].integer() == 15);
    CHECK(response(msgs, 11)["body"]["result"].str() == "7");

    const Json& trace = response(msgs, 14)["body"]["stackFrames"];
    REQUIRE(trace.size() == 2);
    CHECK(trace[0]["name"].str() == "divide");
    CHECK(trace[0]["line"].integer() == 7);
    CHECK(trace[1]["line"].integer() == 20);
    CHECK(variablesText(response(msgs, 16)) == "a=18:int;b=0:int;");
    CHECK(response(msgs, 17)["body"]["description"].str().find("division by zero") != std::string_view::npos);
    CHECK(response(msgs, 18)["body"]["result"].str() == "4");

    std::vector<const Json*> stops = events(msgs, "stopped");
    REQUIRE(stops.size() == 4);
    CHECK((*stops[1])["body"]["reason"].str() == "step");
    CHECK((*stops[2])["body"]["reason"].str() == "breakpoint");
    CHECK((*stops[3])["body"]["reason"].str() == "exception");
    bool printed = false;
    for (const Json* o : events(msgs, "output")) {
        printed = printed || (*o)["body"]["output"].str() == "18\n";
    }
    CHECK(printed);
}

constexpr std::string_view kTasks = R"(fn bump(n: int) -> int {
    return n + 1
}

suspend fn ticker() {
    var i = 0
    while i < 2 {
        wait 2.ticks
        i = bump(i)
    }
}

suspend fn main() {
    var label = "start"
    wait 3.ticks
    print("{label}")
}
)";

std::vector<Json> runSession(const fs::path& dir, const std::string& in) {
    std::ofstream(dir / "in", std::ios::binary) << in;
    std::string cmd = std::string("\"") + QUARTZ_CLI_PATH + "\" debug --dap < \"" + (dir / "in").string() + "\" > \"" +
                      (dir / "out").string() + "\" 2> \"" + (dir / "err").string() + "\"";
    REQUIRE(std::system(cmd.c_str()) == 0);
    std::stringstream ss;
    ss << std::ifstream(dir / "out", std::ios::binary).rdbuf();
    return readFrames(ss.str());
}

TEST_CASE("scripted debug session: tasks as threads") {
    fs::path dir = fs::path(QUARTZ_TEST_DIR) / "dap-tasks";
    fs::create_directories(dir);
    fs::path source = fs::absolute(dir / "main.qz");
    std::ofstream(source, std::ios::binary) << kTasks;
    Json path = Json::object().set("path", source.string());
    Json tasks = Json::array();
    tasks.push("ticker");
    seq = 0;
    Json bps = Json::array();
    bps.push(Json::object().set("line", 9));
    std::string in;
    in += request("initialize", Json::object().set("adapterID", "quartz"));
    in += request("launch", Json::object().set("program", source.string()).set("tasks", tasks));
    in += request("setBreakpoints", Json::object().set("source", path).set("breakpoints", bps));
    in += request("configurationDone", Json::object());
    in += request("stackTrace", Json::object().set("threadId", 2));
    in += request("threads", Json::object());
    in += request("stackTrace", Json::object().set("threadId", 1));
    in += request("setBreakpoints", Json::object().set("source", path).set("breakpoints", Json::array()));
    in += request("next", Json::object().set("threadId", 2));
    in += request("stackTrace", Json::object().set("threadId", 2));
    in += request("continue", Json::object().set("threadId", 2));
    std::vector<Json> msgs = runSession(dir, in);

    const Json& threads = response(msgs, 6)["body"]["threads"];
    REQUIRE(threads.size() == 2);
    CHECK(threads[0]["name"].str() == "main");
    CHECK(threads[1]["name"].str() == "ticker");
    CHECK(response(msgs, 5)["body"]["stackFrames"][0]["line"].integer() == 9);
    const Json& waiting = response(msgs, 7)["body"]["stackFrames"];
    REQUIRE(waiting.size() == 1);
    CHECK(waiting[0]["name"].str() == "main");
    CHECK(waiting[0]["line"].integer() == 15);
    CHECK(response(msgs, 10)["body"]["stackFrames"][0]["line"].integer() == 8);
    std::vector<const Json*> stops = events(msgs, "stopped");
    REQUIRE(stops.size() == 2);
    CHECK((*stops[0])["body"]["threadId"].integer() == 2);
    CHECK((*stops[1])["body"]["reason"].str() == "step");
    CHECK(events(msgs, "thread").size() == 4);
}

constexpr std::string_view kTests = R"(import org.quartzlang.text.*

fn half(n: int) -> int {
    let h = n / 2
    return h
}

@test
fn first() {
    var word = "hepl"
    let n = half(10)
    let near = closest(word, ["help", "warp"]) ?? "none"
    print("{n} {word} {near}")
}

@test
fn panics() {
    panic("boom")
}

@test
fn divides() {
    let z = half(1)
    print("{1 / z}")
}
)";

TEST_CASE("scripted debug session: function breakpoints and trap filters") {
    fs::path dir = fs::path(QUARTZ_TEST_DIR) / "dap-tests";
    fs::create_directories(dir);
    fs::path source = fs::absolute(dir / "main.qz");
    std::ofstream(source, std::ios::binary) << kTests;
    seq = 0;
    Json functions = Json::array();
    functions.push(Json::object().set("name", "half").set("condition", "n == 10"));
    functions.push(Json::object().set("name", "nowhere"));
    Json filters = Json::array();
    filters.push("divideByZero");
    Json path = Json::object().set("path", source.string());
    std::int64_t textRef = 0;
    for (std::size_t i = 0; i < quartz::stdlib::sources().size(); ++i) {
        textRef = quartz::stdlib::sources()[i].first == "<std>/text.qz" ? static_cast<std::int64_t>(i + 1) : textRef;
    }
    std::string in;
    in += request("initialize", Json::object().set("adapterID", "quartz"));
    in += request("launch", Json::object().set("program", source.string()).set("tests", true));
    in += request("setFunctionBreakpoints", Json::object().set("breakpoints", functions));
    in += request("setExceptionBreakpoints", Json::object().set("filters", filters));
    in += request("configurationDone", Json::object());
    in += request("stackTrace", Json::object().set("threadId", 1));
    in += request("scopes", Json::object().set("frameId", 2));
    in += request("setVariable",
                  Json::object().set("variablesReference", 2).set("name", "word").set("value", "\"help\""));
    in += request("setExpression", Json::object().set("expression", "n").set("value", "20").set("frameId", 1));
    in += request("setVariable", Json::object().set("variablesReference", 2).set("name", "word").set("value", "12"));
    Json line = Json::array();
    line.push(Json::object().set("line", 12));
    in += request("setBreakpoints", Json::object().set("source", path).set("breakpoints", line));
    in += request("continue", Json::object().set("threadId", 1));
    in += request("stepIn", Json::object().set("threadId", 1));
    in += request("stackTrace", Json::object().set("threadId", 1));
    in += request("source", Json::object().set("sourceReference", textRef));
    in += request("setBreakpoints", Json::object().set("source", path).set("breakpoints", Json::array()));
    in += request("continue", Json::object().set("threadId", 1));
    in += request("exceptionInfo", Json::object().set("threadId", 3));
    in += request("continue", Json::object().set("threadId", 3));
    std::vector<Json> msgs = runSession(dir, in);

    CHECK(response(msgs, 1)["body"]["supportsFunctionBreakpoints"].boolean());
    const Json& set = response(msgs, 3)["body"]["breakpoints"];
    REQUIRE(set.size() == 2);
    CHECK(set[0]["verified"].boolean());
    CHECK(set[0]["line"].integer() == 4);
    CHECK_FALSE(set[1]["verified"].boolean());
    const Json& top = response(msgs, 6)["body"]["stackFrames"];
    REQUIRE(top.size() == 2);
    CHECK(top[0]["name"].str() == "half");
    CHECK(top[0]["line"].integer() == 4);
    CHECK(response(msgs, 8)["body"]["value"].str() == "\"help\"");
    CHECK(response(msgs, 9)["body"]["value"].str() == "20");
    CHECK_FALSE(response(msgs, 10)["success"].boolean());
    const Json& library = response(msgs, 14)["body"]["stackFrames"][0];
    CHECK(library["name"].str() == "closest");
    CHECK(library["source"]["sourceReference"].integer() == textRef);
    CHECK(response(msgs, 15)["body"]["content"].str().find("pub fn closest") != std::string_view::npos);
    CHECK(response(msgs, 18)["body"]["exceptionId"].str() == "divideByZero");
    bool printed = false;
    for (const Json* o : events(msgs, "output")) {
        printed = printed || (*o)["body"]["output"].str() == "10 help help\n";
    }
    CHECK(printed);
    std::vector<const Json*> stops = events(msgs, "stopped");
    REQUIRE(stops.size() == 4);
    CHECK((*stops[0])["body"]["reason"].str() == "function breakpoint");
    CHECK((*stops[2])["body"]["reason"].str() == "step");
    CHECK((*stops[3])["body"]["reason"].str() == "exception");
    CHECK((*stops[3])["body"]["threadId"].integer() == 3);
}

TEST_CASE("the profiler counts calls and fuel exactly") {
    quartz::SourceInput src[] = {{"p.qz", R"(fn fib(n: int) -> int {
    if n < 2 {
        return n
    }
    return fib(n - 1) + fib(n - 2)
}
fn sum(n: int) -> int {
    var s = 0
    for i in 0..n {
        s += i
    }
    return s
}
fn run() -> int {
    return fib(10) + sum(10)
}
)"}};
    quartz::CompileOptions opts;
    opts.probes.calls = true;
    quartz::CompileOutput out = quartz::compile(src, opts);
    REQUIRE_MESSAGE(out.program, out.diagnostics);
    quartz::vm::Sandbox sb(out.program);
    quartz::vm::Profiler profiler;
    profiler.attach(sb);
    REQUIRE(sb.start().has_value());
    auto r = sb.call(out.program->findFunction("run"), {});
    REQUIRE(r.has_value());
    CHECK(r->i == 100);
    auto stats = profiler.functions();
    REQUIRE(stats.size() == 3);
    auto of = [&](std::string_view name) {
        for (const auto& s : stats) {
            if (out.program->functions[s.function].name == name) {
                return s;
            }
        }
        FAIL("missing ", name);
        return stats[0];
    };
    // One unit per call made and per loop back edge; fib(10) makes 2 * fib(11) - 1 calls.
    CHECK(of("fib").calls == 177);
    CHECK(of("fib").selfFuel == 176);
    CHECK(of("sum").selfFuel == 9);
    CHECK(of("run").selfFuel == 2);
    CHECK(of("run").totalFuel == sb.fuelPerCall() - sb.fuelLeft());
    CHECK(profiler.speedscope(*out.program).find(R"("name":"fib")") != std::string::npos);
}
