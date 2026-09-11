package org.quartzlang.jvm;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import org.junit.jupiter.api.Test;

class ValuesTest {
    static final String HOST = """
            @derive(Eq, Hash)
            struct Pos {
                x: int
                y: int
            }
            struct Request {
                url: str
                timeout: int
            }
            struct Response {
                status: int
                body: str
            }
            fn send(req: Request) -> Result<Response, str>
            fn lookup(id: int) -> int?
            fn tally(scores: Map<str, int>) -> Map<Pos, str>
            @handle
            @awaitable(Pos)
            struct AskPos {}
            fn askPos() -> AskPos
            fn echo(data: Bytes) -> Bytes
            """;
    static final String PLUGIN = """
            struct Point {
                x: int
                y: int
                label: str
            }
            enum Color {
                Red
                Green
                DeepBlue
            }
            enum Shape {
                Circle(float)
                Rect(float, float)
                Empty
            }
            fn shift(p: Point, dx: int) -> Point {
                return Point(p.x + dx, p.y, "{p.label}!")
            }
            fn swap(t: (int, str)) -> (str, int) {
                return (t.1, t.0)
            }
            fn next(c: Color) -> Color {
                return match c {
                    Red => Color.Green
                    Green => Color.DeepBlue
                    DeepBlue => Color.Red
                }
            }
            fn grow(s: Shape) -> Shape {
                return match s {
                    Circle(r) => Shape.Rect(r, r * 2.0)
                    Rect(w, h) => Shape.Circle(w + h)
                    Empty => Shape.Empty
                }
            }
            fn half(n: int) -> int? {
                if n % 2 == 0 {
                    return n / 2
                }
                return null
            }
            fn orZero(n: int?) -> int {
                return n ?? 0
            }
            fn found(id: int) -> int {
                return lookup(id) ?? -1
            }
            fn grid(words: [str]) -> Map<Pos, str> {
                var m: Map<Pos, str> = Map()
                for i in 0..words.len() {
                    m[Pos(i, i * 10)] = words[i]
                }
                return m
            }
            fn count(scores: Map<str, int>) -> int {
                var n = 0
                for k, v in tally(scores) {
                    n += k.x + k.y + v.len()
                }
                return n
            }
            fn fetch(url: str) -> str {
                return match send(Request(url, 5)) {
                    Ok(r) => "{r.status} {r.body}"
                    Err(e) => "error {e}"
                }
            }
            fn parse(s: str) -> Result<int, str> {
                if s == "" {
                    return Err("empty")
                }
                return Ok(s.len())
            }
            fn line(n: int) -> [Point] {
                var out: [Point] = []
                for i in 0..n {
                    out.push(Point(i, -i, "p{i}"))
                }
                return out
            }
            fn total(ps: [Point]) -> int {
                var n = 0
                for p in ps {
                    n += p.x - p.y
                }
                return n
            }
            fn bounce(data: Bytes) -> Bytes {
                return echo(data)
            }
            suspend fn walk() -> int {
                let p = wait askPos()
                return p.x * 100 + p.y
            }
            """;

    record Point(String label, long x, long y) {}

    record Pos(long x, long y) {}

    record Pt(String label, long x, long y) {}

    record Request(String url, int timeout) {}

    record Response(int status, String body) {}

    enum Color {
        RED,
        GREEN,
        DEEP_BLUE
    }

    sealed interface Shape {
        record Circle(double radius) implements Shape {}

        record Rect(double w, double h) implements Shape {}

        record Empty() implements Shape {}
    }

    @Test
    void compositeValuesCrossBothWays() {
        try (Program program = Quartz.compile(Source.of("host.qzi", HOST), Source.of("plugin.qz", PLUGIN));
                Sandbox sandbox = new Sandbox(program)) {
            sandbox.bind("send", call -> {
                Request r = call.arg(0, Request.class);
                return r.url().isEmpty() ? Result.err("no url") : new Response(200, r.url() + "/" + r.timeout());
            });
            sandbox.bind("lookup", call -> call.longArg(0) > 0 ? call.longArg(0) * 2 : null);
            sandbox.bind("tally", call -> {
                Map<Object, Object> out = new LinkedHashMap<>();
                call.mapArg(0).forEach((k, v) -> out.put(new Pos((Long) v, 1), k));
                return out;
            });
            sandbox.bind("askPos", call -> 1L);
            sandbox.bind("echo", call -> call.bytesArg(0));
            sandbox.start();

            Struct moved = sandbox.call("shift", Map.of("x", 1L, "y", 2L, "label", "a"), 10).asStruct();
            assertEquals(List.of("x", "y", "label"), moved.names());
            assertEquals(11L, moved.get("x"));
            assertEquals(new Point("b!", 3, 4), sandbox.call("shift", new Point("b", 1, 4), 2).to(Point.class));
            assertThrows(IllegalArgumentException.class, () -> sandbox.call("shift", Map.of("x", 1L), 1));
            assertThrows(IllegalArgumentException.class, () -> sandbox.call("shift", new Pt("c", 1, 1), 1));
            byte[] binary = {(byte) 0xff, 0, (byte) 0xc3, (byte) 0x28};
            assertArrayEquals(binary, sandbox.call("bounce", binary).asBytes());

            assertEquals(List.of("s", 7L), sandbox.call("swap", List.of(7L, "s")).asTuple());
            assertEquals(Color.DEEP_BLUE, sandbox.call("next", Color.GREEN).to(Color.class));
            assertEquals(new Variant("Red", List.of()), sandbox.call("next", "DeepBlue").asVariant());
            assertEquals(new Shape.Rect(1.5, 3.0), sandbox.call("grow", new Shape.Circle(1.5)).to(Shape.class));
            assertEquals(Variant.of("Circle", 4.0), sandbox.call("grow", Variant.of("Rect", 1.0, 3.0)).get());
            assertEquals(new Shape.Empty(), sandbox.call("grow", new Shape.Empty()).to(Shape.class));

            assertEquals(3L, sandbox.call("half", 6).get());
            assertTrue(sandbox.call("half", 5).isNull());
            assertEquals(9L, sandbox.call("orZero", 9).asLong());
            assertEquals(0L, sandbox.call("orZero", (Object) null).asLong());
            assertEquals(14L, sandbox.call("found", 7).asLong());
            assertEquals(-1L, sandbox.call("found", 0).asLong());

            Map<Object, Object> grid = sandbox.call("grid", List.of("a", "b")).asMap();
            assertEquals(List.of("a", "b"), List.copyOf(grid.values()));
            Map<Pos, String> typed = sandbox.call("grid", List.of("a", "b")).to(new TypeRef<Map<Pos, String>>() {});
            assertEquals("b", typed.get(new Pos(1, 10)));
            assertEquals(11L, sandbox.call("count", Map.of("one", 1L, "abc", 2L)).asLong());

            assertEquals("200 x.org/5", sandbox.call("fetch", "x.org").asString());
            assertEquals("error no url", sandbox.call("fetch", "").asString());
            assertEquals(Result.ok(3L), sandbox.call("parse", "abc").asResult());
            ResultException err = assertThrows(ResultException.class,
                    () -> sandbox.call("parse", "").asResult().orElseThrow());
            assertEquals("empty", err.error());

            List<Point> line = sandbox.call("line", 3).to(new TypeRef<List<Point>>() {});
            assertEquals(new Point("p2", 2, -2), line.get(2));
            assertEquals(6L, sandbox.call("total", line).asLong());

            TaskOutcome waiting = sandbox.spawn("walk");
            assertEquals(TaskOutcome.Kind.WAIT, waiting.kind());
            assertEquals(304L, sandbox.resume(waiting.task(), new Pos(3, 4)).value().asLong());
            assertNull(sandbox.call("half", 1).to(Long.class));
        }
    }
}
