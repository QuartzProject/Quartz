package org.quartzlang.kmp

import kotlinx.coroutines.test.runTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertTrue

class QuartzTest {
    private val host = "fn greet(name: str) -> str\nfn fail(code: int) -> int\nfn sum(xs: [int]) -> int\n" +
        "fn names(n: int) -> [str]\n"
    private val plugin = """
        state Counter {
            total: int
        }
        fn add(c: Counter, n: int) -> int {
            c.total = c.total + n
            return c.total
        }
        fn hello(name: str, times: int) -> str {
            return greet("{name}x{times}")
        }
        fn divide(a: int, b: int) -> int {
            return a / b
        }
        fn check(code: int) -> int {
            return fail(code) + 1
        }
        fn total(xs: [int]) -> int {
            return sum(xs)
        }
        fn joined(n: int) -> str {
            var out = ""
            for s in names(n) {
                out = out + s
            }
            return out
        }
        fn pair(a: int) -> (int, int) {
            return (a * 2, a + 1)
        }
        fn words(n: int) -> [str] {
            var out: [str] = []
            var i = 0
            while i < n {
                out.push("w{i}")
                i += 1
            }
            return out
        }
    """.trimIndent()

    private fun started(program: Program): Sandbox = Quartz.sandbox(program).apply {
        bind("greet") { "hi " + it.string(0) }
        bind("fail") { if (it.long(0) != 0L) throw IllegalStateException("boom ${it.long(0)}") else 0L }
        bind("sum") { call -> call.list(0).sumOf { it as Long } }
        bind("names") { call -> List(call.int(0)) { "n$it" } }
        start()
    }

    @Test
    fun callsAreTypedAndTrapsKeepTheSandbox() = runTest {
        Quartz.initialize()
        compileQuartz("host.qzi" to host, "plugin.qz" to plugin).use { program ->
            started(program).use { sandbox ->
                assertEquals(5L, sandbox.callAs<Long>("add", 5))
                assertEquals(12L, sandbox.call("add", 7).get())
                assertEquals("hi zoë x3", sandbox.callAs<String>("hello", "zoë ", 3))
                assertEquals(Kind.STR, program.export("hello").signature.result.kind)
                assertFailsWith<IllegalArgumentException> { sandbox.call("hello", 3, 3) }
                assertFailsWith<IllegalArgumentException> { sandbox.call("divide", 1) }
                val division = assertFailsWith<TrapException> { sandbox.call("divide", 7, 0) }
                assertEquals(TrapCode.DIVIDE_BY_ZERO, division.code)
                val host = assertFailsWith<TrapException> { sandbox.call("check", 9) }
                assertEquals(TrapCode.HOST_ERROR, host.code)
                assertTrue(host.cause is IllegalStateException)
                assertEquals(3L, sandbox.callAs<Long>("divide", 7, 2))
                val words = sandbox.call("words", 3)
                assertEquals(listOf("w0", "w1", "w2"), words.asList())
                assertEquals(6L, sandbox.callAs<Long>("total", listOf(1L, 2L, 3L)))
                assertFailsWith<IllegalArgumentException> { sandbox.call("total", listOf("x")) }
                assertEquals("n0n1", sandbox.callAs<String>("joined", 2))
                assertEquals(listOf(14L, 8L), sandbox.call("pair", 7).slots().toList())
                assertTrue(program.imports.any { it.builtIn } && program.imports.none { it.name == "greet" && it.builtIn })
                sandbox.call("add", 0)
                assertFailsWith<IllegalStateException> { words.asList() }
            }
            assertFailsWith<CompileException> { compileQuartz("bad.qz" to "fn f() -> int {\n    return \"x\"\n}\n") }
        }
    }

    @Test
    fun tasksHandlesAndReload() = runTest {
        Quartz.initialize()
        val tasks = "@handle\n@awaitable([int])\nstruct Batch {}\nfn batch() -> Batch\n" +
            "@handle\nstruct Player {\n    id: int\n}\n"
        val v1 = """
            @persist(per = Player)
            state Stats {
                joins: int
            }
            fn join(p: Player, s: Stats) -> int {
                s.joins += 1
                return s.joins
            }
            suspend fn total() -> int {
                let got = wait batch()
                var sum = 0
                for x in got {
                    sum += x
                }
                return sum
            }
        """.trimIndent()
        val first = compileQuartz("host.qzi" to tasks, "p.qz" to v1)
        val second = compileQuartz("host.qzi" to tasks, "p.qz" to v1.replace("s.joins += 1", "s.joins += 10"))
        first.withSandbox { sandbox ->
            sandbox.bind("batch") { 5L }
            sandbox.bind("Player.id") { it.long(0) }
            sandbox.start()
            val waiting = sandbox.spawn("total")
            assertEquals(TaskOutcome.Kind.WAIT, waiting.kind)
            assertEquals(15L, sandbox.resume(waiting.task, listOf(4L, 5L, 6L)).value.asLong())
            assertEquals(1L, sandbox.callAs<Long>("join", 7L))
            val saved = sandbox.exportHandle(7)
            sandbox.importHandle(42, saved)
            sandbox.reload(second)
            assertEquals(11L, sandbox.callAs<Long>("join", 42L))
        }
        first.close()
        second.close()
    }
}
