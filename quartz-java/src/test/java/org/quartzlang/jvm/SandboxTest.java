package org.quartzlang.jvm;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import org.junit.jupiter.api.Test;

class SandboxTest {
    private static final String HOST = """
            fn greet(name: str) -> str
            fn fail(code: int) -> int
            """;
    private static final String PLUGIN = """
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
            fn spin(n: int) -> int {
                var i = 0
                while i >= 0 {
                    i += n
                }
                return i
            }
            fn check(code: int) -> int {
                return fail(code) + 1
            }
            """;

    private static Program plugin() {
        return Quartz.compile(Source.of("host.qzi", HOST), Source.of("plugin.qz", PLUGIN));
    }

    private static Sandbox started(Program program, Limits limits) {
        Sandbox sandbox = new Sandbox(program, limits);
        sandbox.bind("greet", call -> "hi " + call.stringArg(0));
        sandbox.bind("fail", call -> {
            if (call.longArg(0) != 0) {
                throw new IllegalStateException("boom " + call.longArg(0));
            }
            return 0L;
        });
        sandbox.start();
        return sandbox;
    }

    @Test
    void compilesAndCallsWithIntsAndStrings() {
        try (Program program = plugin(); Sandbox sandbox = started(program, Limits.DEFAULT)) {
            assertEquals(5, sandbox.call("add", 5).asLong());
            assertEquals(12, sandbox.call("add", 7).asLong());
            assertEquals("hi zoë x3", sandbox.call("hello", "zoë ", 3).asString());
            CompileException bad = assertThrows(CompileException.class,
                    () -> Quartz.compile(Source.of("bad.qz", "fn f() -> int {\n    return \"x\"\n}\n")));
            assertTrue(bad.diagnostics().contains("error"), bad.diagnostics());
        }
    }

    @Test
    void callsAreCheckedAgainstTheSignature() {
        try (Program program = plugin(); Sandbox sandbox = started(program, Limits.DEFAULT)) {
            Export hello = program.export("hello");
            assertEquals(List.of(new Type(Kind.STR, 1), new Type(Kind.I64, 1)), hello.signature().params());
            assertEquals(Kind.STR, hello.signature().result().kind());
            Import greet = program.findImport("greet").orElseThrow();
            assertEquals(new Type(Kind.STR, 1), greet.signature().result());
            assertThrows(IllegalArgumentException.class, () -> sandbox.call("hello", 3, 3));
            assertThrows(IllegalArgumentException.class, () -> sandbox.call("hello", "x"));
            assertThrows(IllegalArgumentException.class, () -> sandbox.call("divide", 1.5, 2));
            Value n = sandbox.call("divide", 9, 3);
            assertEquals(Kind.I64, n.kind());
            assertThrows(IllegalStateException.class, n::asString);
            assertEquals("hi ax1", sandbox.call("hello", "a", 1).get());
        }
    }

    @Test
    void stringResultsCannotOutliveTheNextCall() {
        try (Program program = plugin(); Sandbox sandbox = started(program, Limits.DEFAULT)) {
            Value first = sandbox.call("hello", "a", 1);
            sandbox.call("add", 1);
            assertThrows(IllegalStateException.class, first::asString);
        }
    }

    @Test
    void trapsLeaveTheSandboxUsable() {
        try (Program program = plugin(); Sandbox sandbox = started(program, Limits.DEFAULT.withFuel(100_000))) {
            TrapException division = assertThrows(TrapException.class, () -> sandbox.call("divide", 7, 0));
            assertEquals(TrapCode.DIVIDE_BY_ZERO, division.code());
            assertEquals(3, sandbox.call("divide", 7, 2).asLong());
            TrapException fuel = assertThrows(TrapException.class, () -> sandbox.call("spin", 1));
            assertEquals(TrapCode.OUT_OF_FUEL, fuel.code());
            assertEquals(4, sandbox.call("divide", 8, 2).asLong());
        }
    }

    @Test
    void hostExceptionsBecomeTraps() {
        try (Program program = plugin(); Sandbox sandbox = started(program, Limits.DEFAULT)) {
            TrapException trap = assertThrows(TrapException.class, () -> sandbox.call("check", 9));
            assertTrue(trap.getMessage().contains("boom 9"), trap.getMessage());
            assertInstanceOf(IllegalStateException.class, trap.getCause());
            assertEquals(TrapCode.HOST_ERROR, trap.code());
            assertEquals(1, sandbox.call("check", 0).asLong());
        }
    }

    @Test
    void stateSurvivesExportAndImport() {
        try (Program program = plugin(); Sandbox first = started(program, Limits.DEFAULT)) {
            first.call("add", 40);
            String json = first.exportState();
            try (Sandbox second = started(program, Limits.DEFAULT)) {
                second.importState(json);
                assertEquals(42, second.call("add", 2).asLong());
            }
        }
    }

    @Test
    void aWaitingTaskResumesWithTheHostValue() {
        String host = "@handle\n@awaitable(int)\nstruct Ticket {}\nfn ticket() -> Ticket\n";
        String plugin = "suspend fn twice() -> int {\n    let got = wait ticket()\n    return got * 2\n}\n";
        try (Program program = Quartz.compile(Source.of("host.qzi", host), Source.of("task.qz", plugin));
                Sandbox sandbox = new Sandbox(program)) {
            sandbox.bind("ticket", call -> 77L);
            sandbox.start();
            TaskOutcome waiting = sandbox.spawn("twice");
            assertEquals(TaskOutcome.Kind.WAIT, waiting.kind());
            assertEquals(77, waiting.value().asLong());
            TaskOutcome done = sandbox.resume(waiting.task(), 21L);
            assertTrue(done.done());
            assertEquals(42, done.value().asLong());
        }
    }

    @Test
    void listsComeOutAndGoIntoWaitingTasks() {
        String host = "@handle\n@awaitable([int])\nstruct Batch {}\nfn batch() -> Batch\nfn names(n: int) -> [str]\n"
                + "fn sum(xs: [int]) -> int\n";
        String plugin = """
                fn words(n: int) -> [str] {
                    var out: [str] = []
                    var i = 0
                    while i < n {
                        out.push("w{i}")
                        i += 1
                    }
                    return out
                }
                fn joined(n: int) -> str {
                    var out = ""
                    for s in names(n) {
                        out = out + s
                    }
                    return out
                }
                fn total(xs: [int]) -> int {
                    return sum(xs)
                }
                fn pair(a: int) -> (int, int) {
                    return (a * 2, a + 1)
                }
                suspend fn gather() -> int {
                    let got = wait batch()
                    var sum = 0
                    for x in got {
                        sum += x
                    }
                    return sum
                }
                """;
        try (Program program = Quartz.compile(Source.of("host.qzi", host), Source.of("lists.qz", plugin));
                Sandbox sandbox = new Sandbox(program)) {
            sandbox.bind("batch", call -> 5L);
            sandbox.bind("names", call -> List.of("n0", "n1", "n2").subList(0, call.intArg(0)));
            sandbox.bind("sum", call -> call.listArg(0).stream().mapToLong(x -> (Long) x).sum());
            sandbox.start();
            Value words = sandbox.call("words", 3);
            assertEquals(3, words.listLength());
            assertEquals(List.of("w0", "w1", "w2"), words.asStringList());
            assertThrows(IllegalStateException.class, words::asLongList);
            assertEquals("n0n1", sandbox.call("joined", 2).asString());
            assertEquals(6, sandbox.call("total", List.of(1L, 2L, 3L)).asLong());
            assertThrows(IllegalArgumentException.class, () -> sandbox.call("total", List.of("x")));
            assertArrayEquals(new long[] {14, 8}, sandbox.call("pair", 7).slots());
            assertTrue(program.imports().stream().anyMatch(Import::builtIn));
            TaskOutcome waiting = sandbox.spawn("gather");
            assertEquals(1, sandbox.liveTasks());
            TaskOutcome done = sandbox.resume(waiting.task(), List.of(4L, 5L, 6L));
            assertEquals(15, done.value().asLong());
        }
    }

    @Test
    void reloadKeepsStateAndHandlesPersistPerHandle() {
        String host = "@handle\nstruct Player {\n    id: int\n}\n";
        String v1 = """
                @persist(per = Player)
                state Stats {
                    joins: int
                }
                fn join(p: Player, s: Stats) -> int {
                    s.joins += 1
                    return s.joins
                }
                """;
        try (Program first = Quartz.compile(Source.of("host.qzi", host), Source.of("p.qz", v1));
                Program second = Quartz.compile(Source.of("host.qzi", host),
                        Source.of("p.qz", v1.replace("s.joins += 1", "s.joins += 10")));
                Sandbox sandbox = new Sandbox(first)) {
            sandbox.bind("Player.id", call -> call.longArg(0));
            sandbox.start();
            assertEquals(Kind.HANDLE, first.export("join").signature().params().get(0).kind());
            assertEquals(1, sandbox.call("join", 7L).asLong());
            String saved = sandbox.exportHandle(7);
            sandbox.dropHandle(7);
            sandbox.importHandle(42, saved);
            assertEquals(2, sandbox.call("join", 42L).asLong());
            sandbox.reload(second);
            assertEquals(second, sandbox.program());
            assertEquals(12, sandbox.call("join", 42L).asLong());
        }
    }

    @Test
    void linkedSandboxesCallEachOther() {
        String economy = """
                package org.example.economy
                state Bank {
                    money: Map<str, int>
                }
                fn balance(bank: Bank, who: str) -> int {
                    return bank.money.get(who) ?? 100
                }
                fn pay(bank: Bank, who: str, amount: int) -> str {
                    bank.money[who] = balance(bank, who) + amount
                    return "{who}={bank.money[who]}"
                }
                """;
        String iface = "package org.example.economy\n\nfn balance(a0: str) -> int\nfn pay(a0: str, a1: int) -> str\n";
        String shop = """
                import org.example.economy.*
                fn buy(who: str) -> str {
                    let receipt = pay(who, 0 - 30)
                    return "{receipt} left {balance(who)}"
                }
                """;
        String manifest = "name = \"org.example.economy\"\nversion = \"1.0.0\"\nexports = [\"balance\", \"pay\"]\n";
        try (Program eco = Quartz.compile(Source.of("eco.qz", economy));
                Program client = Quartz.compile(Source.of("economy.qzi", iface), Source.of("shop.qz", shop));
                Sandbox user = new Sandbox(client)) {
            String generated = eco.packageInterface(manifest);
            assertTrue(generated.contains("fn pay("), generated);
            Sandbox provider = new Sandbox(eco);
            user.link("org.example.economy", provider);
            provider.start();
            user.start();
            assertEquals("ana=70 left 70", user.call("buy", "ana").asString());
            provider.close();
            assertEquals("ana=40 left 40", user.call("buy", "ana").asString());
        }
    }

    @Test
    void sandboxesSharingAProgramRunOnSeparateThreads() throws Exception {
        try (Program program = plugin(); ExecutorService pool = Executors.newFixedThreadPool(3)) {
            List<Future<Long>> results = new ArrayList<>();
            for (int t = 0; t < 3; ++t) {
                final long step = t + 1;
                results.add(pool.submit(() -> {
                    try (Sandbox sandbox = started(program, Limits.DEFAULT)) {
                        long last = 0;
                        for (int i = 0; i < 2_000; ++i) {
                            last = sandbox.call("add", step).asLong();
                            assertEquals("hi tx" + i, sandbox.call("hello", "t", i).asString());
                        }
                        return last;
                    }
                }));
            }
            for (int t = 0; t < 3; ++t) {
                assertEquals(2_000L * (t + 1), results.get(t).get());
            }

            CountDownLatch inside = new CountDownLatch(1);
            CountDownLatch release = new CountDownLatch(1);
            try (Sandbox shared = new Sandbox(program)) {
                shared.bind("greet", call -> {
                    inside.countDown();
                    release.await();
                    return "late";
                });
                shared.bind("fail", call -> 0L);
                shared.start();
                Future<String> busy = pool.submit(() -> shared.call("hello", "x", 1).asString());
                inside.await();
                assertThrows(IllegalStateException.class, () -> shared.call("add", 1));
                release.countDown();
                assertEquals("late", busy.get());
            }
        }
    }
}
