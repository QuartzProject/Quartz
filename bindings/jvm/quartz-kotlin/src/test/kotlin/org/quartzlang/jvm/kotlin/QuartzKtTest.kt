package org.quartzlang.jvm.kotlin

import kotlin.test.Test
import kotlin.test.assertEquals
import org.quartzlang.jvm.Result

class QuartzKtTest {
    data class Point(val label: String, val x: Long, val y: Long)

    enum class Color { RED, GREEN, DEEP_BLUE }

    sealed interface Shape {
        data class Circle(val radius: Double) : Shape

        data class Rect(val w: Double, val h: Double) : Shape

        data object Empty : Shape
    }

    @Test
    fun callsReadTheDeclaredType() {
        val program = compileQuartz(
            "host.qzi" to "fn shout(text: str) -> str\nfn near(p: Point) -> Result<Point, str>\n" +
                "struct Point {\n    x: int\n    y: int\n    label: str\n}\n",
            "plugin.qz" to """
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
                fn twice(n: int) -> int {
                    return n * 2
                }
                fn loud(s: str) -> str {
                    return shout(s)
                }
                fn moved(p: Point) -> Result<Point, str> {
                    return near(Point(p.x + 1, p.y, p.label))
                }
                fn row(n: int) -> [Point] {
                    var out: [Point] = []
                    for i in 0..n {
                        out.push(Point(i, i * i, "r{i}"))
                    }
                    return out
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
                        Circle(r) => Shape.Rect(r, r)
                        Rect(w, h) => Shape.Empty
                        Empty => Shape.Circle(1.0)
                    }
                }
            """.trimIndent() + "\n",
        )
        program.use {
            it.withSandbox { sandbox ->
                sandbox.bind("shout") { call -> call.stringArg(0).uppercase() }
                sandbox.bind("near") { call ->
                    val p = call.argAs<Point>(0)
                    if (p.x > 5) Result.err<Point, String>("far") else p.copy(label = p.label + "+")
                }
                sandbox.start()
                assertEquals(42L, sandbox.callAs<Long>("twice", 21))
                assertEquals("HEY", sandbox.callAs<String>("loud", "hey"))
                val moved = sandbox.callAs<Result<Point, String>>("moved", Point("a", 1, 0))
                assertEquals(Result.ok(Point("a+", 2, 0)), moved)
                assertEquals(Result.err("far"), sandbox.callAs<Result<Point, String>>("moved", Point("b", 9, 0)))
                assertEquals(Point("r2", 2, 4), sandbox.callAs<List<Point>>("row", 3)[2])
                assertEquals(Color.RED, sandbox.callAs<Color>("next", Color.DEEP_BLUE))
                assertEquals(Shape.Rect(2.0, 2.0), sandbox.callAs<Shape>("grow", Shape.Circle(2.0)))
                assertEquals(Shape.Empty, sandbox.callAs<Shape>("grow", Shape.Rect(1.0, 1.0)))
                assertEquals(Shape.Circle(1.0), sandbox.callAs<Shape>("grow", Shape.Empty))
            }
        }
    }

    class Player(val name: String)

    sealed interface Event {
        data class Join(val who: Player) : Event

        data class Leave(val who: Player, val why: String) : Event
    }

    @Test
    fun handlesInsideSealedClassesMapToHostObjects() {
        val players = listOf(Player("ann"), Player("bob"))
        val handles = handleMapper({ id, _ -> players[id.toInt()] }, { p, _ -> players.indexOf(p).toLong() })
        val program = compileQuartz(
            "host.qzi" to "@handle\nstruct Player {}\nenum Event {\n    Join(Player)\n    Leave(Player, str)\n}\n",
            "plugin.qz" to "fn kick(e: Event) -> Event {\n    return match e {\n" +
                "        Join(p) => Event.Leave(p, \"kick\")\n        Leave(p, _) => Event.Join(p)\n    }\n}\n",
        )
        program.use {
            it.withSandbox { sandbox ->
                sandbox.start()
                val event = program.export("kick").signature().params()[0]
                val join = org.quartzlang.jvm.Values.from(Event.Join(players[1]), event, handles)
                assertEquals(Event.Leave(players[1], "kick"), sandbox.call("kick", join).read<Event>(handles))
            }
        }
    }
}
