package org.quartzlang.kmp

import kotlinx.coroutines.test.runTest
import kotlinx.serialization.Serializable
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertNull
import kotlin.test.assertTrue

class ValuesTest {
    @Serializable
    data class Point(val label: String, val x: Long, val y: Long)

    @Serializable
    data class Request(val url: String, val timeout: Int)

    @Serializable
    data class Response(val status: Int, val body: String)

    @Serializable
    enum class Color { RED, GREEN, DEEP_BLUE }

    @Serializable
    sealed interface Shape {
        @Serializable
        data class Circle(val radius: Double) : Shape

        @Serializable
        data class Rect(val w: Double, val h: Double) : Shape

        @Serializable
        data object Empty : Shape
    }

    private val host = """
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
    """.trimIndent() + "\n"

    private val plugin = """
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
        suspend fn walk() -> int {
            let p = wait askPos()
            return p.x * 100 + p.y
        }
    """.trimIndent() + "\n"

    @Test
    fun compositeValuesCrossBothWays() = runTest {
        Quartz.initialize()
        compileQuartz("host.qzi" to host, "plugin.qz" to plugin).use { program ->
            program.withSandbox { sandbox ->
                sandbox.bind("send") { call ->
                    val r = call.decode<Request>(0)
                    if (r.url.isEmpty()) Result.Err("no url") else quartzValue(Response(200, "${r.url}/${r.timeout}"))
                }
                sandbox.bind("lookup") { call -> call.long(0).takeIf { it > 0 }?.times(2) }
                sandbox.bind("tally") { call ->
                    call.map(0).entries.associate { (k, v) -> Struct("x" to v, "y" to 1L) to k }
                }
                sandbox.bind("askPos") { 1L }
                sandbox.start()

                val moved = sandbox.call("shift", mapOf("x" to 1L, "y" to 2L, "label" to "a"), 10).asStruct()
                assertEquals(listOf("x", "y", "label"), moved.names)
                assertEquals(11L, moved["x"])
                assertEquals(Point("b!", 3, 4), sandbox.callAs<Point>("shift", quartzValue(Point("b", 1, 4)), 2))
                assertFailsWith<IllegalArgumentException> { sandbox.call("shift", mapOf("x" to 1L), 1) }
                assertFailsWith<IllegalArgumentException> { sandbox.call("shift", Point("c", 1, 1), 1) }

                assertEquals(listOf("s", 7L), sandbox.call("swap", 7L to "s").asTuple())
                assertEquals(Color.DEEP_BLUE, sandbox.callAs<Color>("next", Color.GREEN))
                assertEquals(Variant("Red"), sandbox.call("next", "DeepBlue").asVariant())
                assertEquals(Shape.Rect(1.5, 3.0), sandbox.callAs<Shape>("grow", quartzValue<Shape>(Shape.Circle(1.5))))
                assertEquals(Variant.of("Circle", 4.0), sandbox.call("grow", Variant.of("Rect", 1.0, 3.0)).get())
                assertEquals(Shape.Empty, sandbox.callAs<Shape>("grow", quartzValue<Shape>(Shape.Empty)))

                assertEquals(3L, sandbox.call("half", 6).get())
                assertTrue(sandbox.call("half", 5).isNull)
                assertEquals(9L, sandbox.callAs<Long>("orZero", 9))
                assertEquals(0L, sandbox.callAs<Long>("orZero", null))
                assertEquals(14L, sandbox.callAs<Long>("found", 7))
                assertEquals(-1L, sandbox.callAs<Long>("found", 0))

                assertEquals(listOf("a", "b"), sandbox.call("grid", listOf("a", "b")).asMap().values.toList())
                assertEquals("b", sandbox.call("grid", listOf("a", "b")).asMap()[Struct("x" to 1L, "y" to 10L)])
                assertEquals(11L, sandbox.callAs<Long>("count", mapOf("one" to 1L, "abc" to 2L)))

                assertEquals("200 x.org/5", sandbox.callAs<String>("fetch", "x.org"))
                assertEquals("error no url", sandbox.callAs<String>("fetch", ""))
                assertEquals(Result.Ok(3L), sandbox.call("parse", "abc").asResult())
                assertEquals(Result.Ok(3), sandbox.callAs<Result<Int, String>>("parse", "abc"))
                val err = assertFailsWith<ResultException> { sandbox.call("parse", "").asResult().getOrThrow() }
                assertEquals("empty", err.error)

                val line = sandbox.callAs<List<Point>>("line", 3)
                assertEquals(Point("p2", 2, -2), line[2])
                assertEquals(6L, sandbox.callAs<Long>("total", quartzValue(line)))

                val waiting = sandbox.spawn("walk")
                assertEquals(TaskOutcome.Kind.WAIT, waiting.kind)
                assertEquals(304L, sandbox.resume(waiting.task, Struct("x" to 3L, "y" to 4L)).value.asLong())
                assertNull(sandbox.call("half", 1).get())
            }
        }
    }
}
