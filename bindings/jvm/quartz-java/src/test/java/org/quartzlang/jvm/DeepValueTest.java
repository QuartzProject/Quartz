package org.quartzlang.jvm;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

class DeepValueTest {
    static final long DEPTH = 200_000;
    static final String SOURCE = """
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
            """;

    sealed interface Chain {
        record End() implements Chain {}

        record Link(long value, Chain next) implements Chain {}
    }

    @Test
    void valuesNestedTwoHundredThousandLevelsCrossBothWays() {
        Limits limits = Limits.DEFAULT.withMemory(1L << 28).withFuel(1L << 40);
        try (Program program = Quartz.compile(Source.of("deep.qz", SOURCE));
                Sandbox sandbox = new Sandbox(program, limits)) {
            sandbox.start();
            Object generic = sandbox.call("build", DEPTH).get();
            assertEquals(DEPTH, sandbox.call("length", generic).asLong());
            Chain chain = sandbox.call("build", DEPTH).to(Chain.class);
            assertEquals(DEPTH - 1, ((Chain.Link) chain).value());
            assertEquals(DEPTH, sandbox.call("length", chain).asLong());
            Object tree = sandbox.call("tree", DEPTH).get();
            assertEquals(DEPTH, sandbox.call("height", Values.from(tree, program.export("height")
                    .signature().params().get(0))).asLong());
        }
    }
}
