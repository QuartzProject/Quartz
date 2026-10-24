package org.quartzlang.kmp

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertFalse
import kotlin.test.assertNotNull
import kotlin.test.assertTrue
import kotlinx.coroutines.test.runTest

class IntrospectionTest {
    @Test
    fun diagnosticsCarryRulesAndRuleLevelsDecideFailure() = runTest {
        Quartz.initialize()
        val lint = "lint.qz" to "fn f(x: int) -> int {\n    let y = 1\n    return x\n}\n"
        compileQuartz(lint).use { program ->
            val d = program.diagnostics.single()
            val expected = Diagnostic(Diagnostic.Severity.WARNING, "unused-variable", "lint.qz", 2, 9, 2, 10, d.message)
            assertEquals(expected, d)
            assertTrue("[unused-variable]" in program.diagnosticsText)
        }
        val denied = assertFailsWith<CompileException> {
            val options = CompileOptions().denyWarnings().withRule("unused-parameter", WarningLevel.IGNORE)
            compileQuartz(lint, options = options)
        }
        assertEquals("unused-variable", denied.errors.single().rule)
        compileQuartz(lint, options = CompileOptions().withRule("unused-variable", WarningLevel.IGNORE)).use {
            assertTrue(it.diagnostics.isEmpty())
        }
        assertTrue("unused-variable" in Quartz.warningRules)
    }

    @Test
    fun exportsAttributesHandlesAndWideTaskResults() = runTest {
        Quartz.initialize()
        val host = "package org.example.host\n@handle\nstruct Player {}\n" +
            "impl Player {\n    fn level(self) -> int\n}\n" +
            "enum Event {\n    Join(Player)\n    Leave(Player, str)\n}\nfn unused(n: int)\n"
        val plugin = """
            package org.example.a
            import org.example.host.{Player, Event}
            @command("greet", perm = "x.greet", times = 2)
            pub fn greet(p: Player, times: int) -> int {
                return p.level() * times
            }
            @on(Join, where = p.level() > 3)
            fn joined(p: Player) {}
            fn kick(e: Event) -> Event {
                return match e {
                    Join(p) => Event.Leave(p, "kick")
                    Leave(p, _) => Event.Join(p)
                }
            }
            suspend fn stats(p: Player) -> (Player, int) {
                return (p, p.level())
            }
        """.trimIndent() + "\n"
        compileQuartz("host.qzi" to host, "a.qz" to plugin).use { program ->
            val greet = program.export("greet")
            assertEquals("org.example.a.greet", greet.name)
            assertEquals("greet", greet.simpleName)
            assertEquals(listOf("p", "times"), greet.parameterNames)
            assertEquals(Visibility.PUBLIC, greet.visibility)
            assertEquals("org.example.host.Player", greet.signature.params[0].qualifiedName)
            val command = assertNotNull(greet.attribute("command"))
            assertEquals("greet", command.positional(0)?.asString())
            assertEquals(2L, command.named("times")?.asLong())
            val on = assertNotNull(program.export("joined").attribute("on"))
            assertEquals(Attribute.ArgumentKind.NAME, on.positional(0)?.kind)
            assertNotNull(on.filter)
            assertTrue(program.export("stats").isSuspend)
            assertTrue(program.exports.any { it.simpleName == "kick" })
            assertTrue(program.findImport("Player.level")!!.required)
            assertFalse(program.findImport("unused")!!.required)

            val players = listOf("ann", "bob")
            val handles = object : HandleMapper {
                override fun toObject(id: Long, type: Type): Any? = players[id.toInt()]
                override fun toId(obj: Any, type: Type): Long = players.indexOf(obj).toLong()
            }
            program.withSandbox { sandbox ->
                sandbox.bind("Player.level") { 4L + it.long(0) }
                sandbox.start()
                val event = program.export("kick").signature.params[0]
                val join = QuartzValues.from(Variant("Join", listOf("bob")), event, handles)
                assertEquals(Variant("Join", listOf(1L)), join)
                val kicked = QuartzValues.resolveHandles(sandbox.call("kick", join).get(), event, handles)
                assertEquals(Variant("Leave", listOf("bob", "kick")), kicked)
                val done = sandbox.spawn("stats", 1L)
                assertTrue(done.done)
                assertEquals(listOf(1L, 5L), done.value.get())
            }
        }
    }
}
