package org.quartzlang.kmp

import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertTrue
import kotlinx.coroutines.test.runTest
import kotlinx.serialization.Serializable

class Level5Test {
    @Serializable
    sealed interface Tree {
        @Serializable
        data class Leaf(val value: Long) : Tree

        @Serializable
        data class Node(val left: Tree, val right: Tree) : Tree
    }

    private val built: Tree = Tree.Node(Tree.Leaf(1), Tree.Node(Tree.Leaf(2), Tree.Leaf(3)))

    private val host = "fn readOutput(raw: Bytes) -> str\nfn header(raw: Bytes) -> [str]\n"

    private val plugin = """
        /// A binary tree of ints.
        @derive(Eq, Hash)
        enum Tree {
            Leaf(int)
            /// Two subtrees.
            Node(Tree, Tree)
        }
        /// Builds a small tree.
        fn build() -> Tree {
            return Tree.Node(Tree.Leaf(1), Tree.Node(Tree.Leaf(2), Tree.Leaf(3)))
        }
        fn sum(t: Tree) -> int {
            return match t {
                Tree.Leaf(v) => v,
                Tree.Node(a, b) => sum(a) + sum(b),
            }
        }
        fn listKeys() -> Map<[int], str> {
            var m: Map<[int], str> = Map()
            m[[1, 2]] = "a"
            return m
        }
        fn lookup(m: Map<[int], str>, k: [int]) -> str {
            return m.get(k) ?? "none"
        }
        fn weigh(m: Map<Tree, int>) -> int {
            return m.get(build()) ?? -1
        }
        fn len(s: str) -> int {
            return s.len()
        }
        fn output(raw: Bytes) -> str {
            return readOutput(raw)
        }
        fn headers(raw: Bytes) -> [str] {
            return header(raw)
        }
    """.trimIndent()

    private fun withProgram(block: (Program, Sandbox) -> Unit) =
        compileQuartz("host.qzi" to host, "plugin.qz" to plugin).use { program ->
            program.withSandbox { sandbox ->
                sandbox.bind("readOutput") { it.stringLossy(it.bytes(0)) }
                sandbox.bind("header") { listOf(LossyText(it.bytes(0)), "ok") }
                sandbox.start()
                block(program, sandbox)
            }
        }

    @Test
    fun recursivePayloadsAndMapKeysCrossAsValues() = runTest {
        Quartz.initialize()
        withProgram { program, sandbox ->
            val tree = program.export("build").signature.result
            val left = tree.findCase("Node")!!.payload[0]
            assertTrue(left.boxed)
            assertEquals(Kind.ENUM, left.type.kind)
            val value = sandbox.call("build").asVariant()
            assertEquals(Variant("Leaf", listOf(1L)), value.values[0])
            assertEquals(built, sandbox.call("build").decode<Tree>())
            assertEquals(6L, sandbox.call("sum", quartzValue(built)).asLong())
            assertEquals(6L, sandbox.call("sum", value).asLong())
            val lists = sandbox.call("listKeys").asMap()
            assertEquals("a", lists[listOf(1L, 2L)])
            assertEquals("a", sandbox.call("lookup", lists, listOf(1L, 2L)).asString())
            assertEquals(42L, sandbox.call("weigh", mapOf(value to 42L)).asLong())
        }
    }

    @Test
    fun lossyTextAndDocs() = runTest {
        Quartz.initialize()
        val raw = byteArrayOf('a'.code.toByte(), 0xff.toByte(), 'b'.code.toByte(), 0xe2.toByte(), 0x82.toByte())
        val text = "a�b�"
        withProgram { program, sandbox ->
            assertEquals(text, sandbox.call("output", raw).asString())
            assertEquals(listOf(text, "ok"), sandbox.call("headers", raw).asList())
            val lossy = sandbox.strLossy(raw)
            assertEquals(text, lossy.asString())
            assertEquals(8L, sandbox.call("len", lossy).asLong())
            val build = program.export("build")
            assertEquals("Builds a small tree.", build.doc)
            assertEquals("A binary tree of ints.", build.signature.result.doc)
            assertEquals("Two subtrees.", build.signature.result.findCase("Node")!!.doc)
            assertEquals("", program.export("sum").doc)
        }
    }

    @Test
    fun compressionRoundTripsInEveryFraming() = runTest {
        Quartz.initialize()
        val data = "abracadabra, abracadabra, abracadabra".encodeToByteArray()
        for (framing in Framing.entries) {
            assertContentEquals(data, Quartz.inflate(Quartz.deflate(data, 9, framing), framing))
        }
        assertFailsWith<QuartzException> { Quartz.inflate(Quartz.deflate(data), maxLength = data.size - 1) }
        assertFailsWith<QuartzException> { Quartz.inflate(byteArrayOf(1, 2, 3), Framing.GZIP) }
        val whole = Quartz.crc32(data)
        assertEquals(reference(data), whole)
        assertEquals(whole, Quartz.crc32(data.copyOfRange(9, data.size), Quartz.crc32(data.copyOf(9))))
    }

    private fun reference(data: ByteArray): Long {
        var crc = 0xFFFF_FFFFL
        for (b in data) {
            crc = crc xor (b.toLong() and 0xFF)
            repeat(8) { crc = if (crc and 1L != 0L) (crc ushr 1) xor 0xEDB8_8320L else crc ushr 1 }
        }
        return crc xor 0xFFFF_FFFFL
    }
}
