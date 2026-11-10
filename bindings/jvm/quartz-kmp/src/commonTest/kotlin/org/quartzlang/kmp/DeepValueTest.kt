package org.quartzlang.kmp

import kotlinx.coroutines.test.runTest
import kotlinx.serialization.Serializable
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith

class DeepValueTest {
    @Serializable
    sealed interface Chain {
        @Serializable
        data object End : Chain

        @Serializable
        data class Link(val value: Long, val next: Chain) : Chain
    }

    private val source = """
        enum Chain {
            End
            Link(int, Chain)
        }
        struct Node {
            v: int
            kids: [Node]
        }
        fn build(n: int) -> Chain {
            var c = Chain.End
            for i in 0..n {
                c = Chain.Link(i, c)
            }
            return c
        }
        fn length(c: Chain) -> int {
            var n = 0
            var cur = c
            var going = true
            while going {
                match cur {
                    End => going = false
                    Link(_, next) => {
                        n += 1
                        cur = next
                    }
                }
            }
            return n
        }
        fn tree(n: int) -> Node {
            var x = Node(0, [])
            for i in 0..n {
                x = Node(i + 1, [x])
            }
            return x
        }
        fn height(t: Node) -> int {
            var h = 0
            var cur = t
            while cur.kids.len() > 0 {
                h += 1
                cur = cur.kids[0]
            }
            return h
        }
    """.trimIndent()

    @Test
    fun valuesNestedTwoHundredThousandLevelsCrossBothWays() = runTest {
        Quartz.initialize()
        val depth = 200_000L
        compileQuartz("deep.qz" to source).use { program ->
            Quartz.sandbox(program, Limits(memoryBytes = 1L shl 28, fuelPerCall = 1L shl 40)).use { sandbox ->
                sandbox.start()
                val chain = sandbox.call("build", depth).get()
                assertEquals(depth - 1, (chain as Variant).values[0])
                assertEquals(depth, sandbox.callAs<Long>("length", chain))
                val tree = sandbox.call("tree", depth).get()
                val node = program.export("height").signature.params[0]
                assertEquals(depth, sandbox.callAs<Long>("height", QuartzValues.from(tree, node)))
                assertFailsWith<QuartzException> { sandbox.call("build", depth).decode<Chain>() }
            }
        }
    }
}
