package org.quartzlang.jvm.kotlin

import kotlin.test.Test
import kotlin.test.assertEquals

class QuartzKtTest {
    @Test
    fun callsReadTheDeclaredType() {
        val program = compileQuartz(
            "host.qzi" to "fn shout(text: str) -> str\n",
            "plugin.qz" to "fn twice(n: int) -> int {\n    return n * 2\n}\n" +
                "fn loud(s: str) -> str {\n    return shout(s)\n}\n",
        )
        program.use {
            it.withSandbox { sandbox ->
                sandbox.bind("shout") { call -> call.stringArg(0).uppercase() }
                sandbox.start()
                assertEquals(42L, sandbox.callAs<Long>("twice", 21))
                assertEquals("HEY", sandbox.callAs<String>("loud", "hey"))
            }
        }
    }
}
