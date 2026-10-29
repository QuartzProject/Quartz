package org.quartzlang.jvm;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.zip.CRC32;
import java.util.zip.GZIPInputStream;
import java.util.zip.GZIPOutputStream;
import org.junit.jupiter.api.Test;

class Level5Test {
    private static final String HOST = """
            fn readOutput(raw: Bytes) -> str
            fn header(raw: Bytes) -> [str]
            """;
    private static final String PLUGIN = """
            /// A binary tree of ints.
            @derive(Eq, Hash, Show)
            pub enum Tree {
                /// A single value.
                Leaf(int)
                /// Two subtrees.
                Node(Tree, Tree)
            }

            /// A point in the plane.
            pub struct Point {
                /// Horizontal.
                x: int
                y: int
            }

            /// Builds a small tree.
            pub fn build() -> Tree {
                return Tree.Node(Tree.Leaf(1), Tree.Node(Tree.Leaf(2), Tree.Leaf(3)))
            }

            pub fn sum(t: Tree) -> int {
                return match t {
                    Tree.Leaf(v) => v,
                    Tree.Node(a, b) => sum(a) + sum(b),
                }
            }

            pub fn origin() -> Point {
                return Point(0, 0)
            }

            pub fn listKeys() -> Map<[int], str> {
                var m: Map<[int], str> = Map()
                m[[1, 2]] = "a"
                m[[3]] = "b"
                return m
            }

            pub fn lookup(m: Map<[int], str>, k: [int]) -> str {
                return m.get(k) ?? "none"
            }

            pub fn treeKeys() -> Map<Tree, int> {
                var m: Map<Tree, int> = Map()
                m[build()] = 6
                m[Tree.Leaf(7)] = 7
                return m
            }

            pub fn weigh(m: Map<Tree, int>) -> int {
                return m.get(build()) ?? -1
            }

            pub fn len(s: str) -> int {
                return s.len()
            }

            pub fn output(raw: Bytes) -> str {
                return readOutput(raw)
            }

            pub fn headers(raw: Bytes) -> [str] {
                return header(raw)
            }
            """;

    sealed interface Tree {
        record Leaf(long value) implements Tree {}

        record Node(Tree left, Tree right) implements Tree {}
    }

    private static final Tree BUILT = new Tree.Node(new Tree.Leaf(1),
            new Tree.Node(new Tree.Leaf(2), new Tree.Leaf(3)));

    private static Program compile() {
        return Quartz.compile(Source.of("host.qzi", HOST), Source.of("plugin.qz", PLUGIN));
    }

    private static Sandbox started(Program program) {
        Sandbox sandbox = new Sandbox(program);
        sandbox.bind("readOutput", call -> call.stringLossy(call.bytesArg(0)));
        sandbox.bind("header", call -> List.of(new LossyText(call.bytesArg(0)), "ok"));
        sandbox.start();
        return sandbox;
    }

    @Test
    void recursivePayloadsReadAndPassAsTheValueItself() {
        try (Program program = compile(); Sandbox sandbox = started(program)) {
            Type tree = program.export("build").signature().result();
            Type.Field left = tree.findCase("Node").payload().get(0);
            assertTrue(left.boxed());
            assertEquals(Kind.LIST, left.box().kind());
            assertEquals(tree, left.type());
            assertFalse(tree.findCase("Leaf").payload().get(0).boxed());

            Variant built = sandbox.call("build").asVariant();
            assertEquals(Variant.of("Leaf", 1L), built.get(0));
            assertEquals(BUILT, sandbox.call("build").to(Tree.class));
            assertEquals(6L, sandbox.call("sum", BUILT).asLong());
            assertEquals(6L, sandbox.call("sum", built).asLong());
            Object generic = Values.from(BUILT, tree);
            assertEquals(built, generic);
            assertEquals(BUILT, Values.to(generic, tree, Tree.class, null));
        }
    }

    @Test
    void listsAndRecursiveEnumsAreMapKeysByContent() {
        try (Program program = compile(); Sandbox sandbox = started(program)) {
            Map<Object, Object> lists = sandbox.call("listKeys").asMap();
            assertEquals("a", lists.get(List.of(1L, 2L)));
            assertEquals("b", sandbox.call("lookup", lists, List.of(3L)).asString());
            Map<Object, Object> trees = sandbox.call("treeKeys").asMap();
            assertEquals(7L, trees.get(Variant.of("Leaf", 7L)));
            Map<Object, Object> keyed = new LinkedHashMap<>();
            keyed.put(BUILT, 42L);
            assertEquals(42L, sandbox.call("weigh", keyed).asLong());
        }
    }

    @Test
    void lossyTextReplacesEachIllFormedSequence() {
        byte[] raw = {'a', (byte) 0xff, 'b', (byte) 0xe2, (byte) 0x82, 'c'};
        try (Program program = compile(); Sandbox sandbox = started(program)) {
            assertEquals("a�b�c", sandbox.call("output", (Object) raw).asString());
            assertEquals(List.of("a�b�c", "ok"), sandbox.call("headers", (Object) raw).asList());
            Value text = sandbox.strLossy(raw);
            assertEquals("a�b�c", text.asString());
            long lossy = sandbox.call("len", text).asLong();
            assertEquals(sandbox.call("len", "a\uFFFDb\uFFFDc").asLong(), lossy);
            assertThrows(IllegalArgumentException.class, () -> sandbox.call("len", new byte[] {1}));
        }
    }

    @Test
    void docsComeFromTheTripleSlashComments() {
        try (Program program = compile()) {
            Export build = program.export("build");
            assertEquals("Builds a small tree.", build.doc());
            assertEquals("", program.export("sum").doc());
            Type tree = build.signature().result();
            assertEquals("A binary tree of ints.", tree.doc());
            assertEquals("Two subtrees.", tree.findCase("Node").doc());
            Type point = program.export("origin").signature().result();
            assertEquals("A point in the plane.", point.doc());
            assertEquals("Horizontal.", point.fields().get(0).doc());
            assertEquals("", point.fields().get(1).doc());
        }
    }

    @Test
    void compressionMatchesJavaUtilZip() throws IOException {
        byte[] text = "the quick brown fox jumps over the lazy dog, again and again and again"
                .getBytes(StandardCharsets.UTF_8);
        CRC32 reference = new CRC32();
        reference.update(text);
        assertEquals(reference.getValue(), Quartz.crc32(0, text));
        long half = Quartz.crc32(0, java.util.Arrays.copyOf(text, 10));
        assertEquals(reference.getValue(), Quartz.crc32(half, java.util.Arrays.copyOfRange(text, 10, text.length)));

        byte[] gz = Quartz.deflate(text, 9, Framing.GZIP);
        try (GZIPInputStream in = new GZIPInputStream(new ByteArrayInputStream(gz))) {
            assertArrayEquals(text, in.readAllBytes());
        }
        ByteArrayOutputStream javaGz = new ByteArrayOutputStream();
        try (GZIPOutputStream out = new GZIPOutputStream(javaGz)) {
            out.write(text);
        }
        assertArrayEquals(text, Quartz.inflate(javaGz.toByteArray(), Framing.GZIP, 1 << 20));
        for (Framing f : Framing.values()) {
            assertArrayEquals(text, Quartz.inflate(Quartz.deflate(text, 6, f), f, text.length));
        }
        assertThrows(QuartzException.class, () -> Quartz.inflate(Quartz.deflate(text, 6, Framing.ZLIB),
                Framing.ZLIB, text.length - 1));
        assertThrows(QuartzException.class, () -> Quartz.inflate(new byte[] {1, 2, 3}, Framing.GZIP, 100));
    }
}
