package org.quartzlang.jvm.bench;

import java.util.Locale;
import org.quartzlang.jvm.Export;
import org.quartzlang.jvm.Limits;
import org.quartzlang.jvm.Program;
import org.quartzlang.jvm.Quartz;
import org.quartzlang.jvm.Sandbox;
import org.quartzlang.jvm.Source;
import org.quartzlang.jvm.Struct;

/** Same workload as bench.c: per call cost of a trivial export, and per upcall cost of a host function. */
public final class Bench {
    static final String HOST = "fn hostId(x: int) -> int\n";
    static final String PLUGIN = """
            fn id(x: int) -> int {
                return x
            }
            fn spin(n: int) -> int {
                var s = 0
                var i = 0
                while i < n {
                    s += i
                    i += 1
                }
                return s
            }
            fn ups(n: int) -> int {
                var s = 0
                var i = 0
                while i < n {
                    s += hostId(i)
                    i += 1
                }
                return s
            }
            struct P {
                x: int
                y: int
                z: int
            }
            fn shift(p: P) -> P {
                return P(p.x + 1, p.y, p.z)
            }
            fn norm(p: P) -> int {
                return p.x + p.y + p.z
            }
            """;

    record P(long x, long y, long z) {}

    private Bench() {}

    public static void main(String[] args) {
        long calls = args.length > 0 ? Long.parseLong(args[0]) : 2_000_000;
        int inner = args.length > 1 ? Integer.parseInt(args[1]) : 1_000;
        int rounds = args.length > 2 ? Integer.parseInt(args[2]) : 5;
        try (Program program = Quartz.compile(Source.of("host.qzi", HOST), Source.of("bench.qz", PLUGIN));
                Sandbox sandbox = new Sandbox(program,
                        Limits.DEFAULT.withTier(Limits.Tier.INTERPRETER).withFuel(1L << 40))) {
            sandbox.bind("hostId", call -> call.longArg(0));
            sandbox.start();
            Export id = program.export("id");
            Export spin = program.export("spin");
            Export ups = program.export("ups");
            Export shift = program.export("shift");
            Export norm = program.export("norm");
            long check = 0;
            for (int round = 0; round < rounds; ++round) {
                check = 0;
                long t0 = System.nanoTime();
                for (long i = 0; i < calls; ++i) {
                    check += sandbox.call(id, i).asLong();
                }
                long t1 = System.nanoTime();
                long loops = calls / inner * 10;
                for (long i = 0; i < loops; ++i) {
                    check += sandbox.call(spin, inner).asLong();
                }
                long t2 = System.nanoTime();
                for (long i = 0; i < loops; ++i) {
                    check += sandbox.call(ups, inner).asLong();
                }
                long t3 = System.nanoTime();
                P p = new P(0, 2, 3);
                for (long i = 0; i < calls; ++i) {
                    check += sandbox.call(norm, p).asLong();
                }
                long t4 = System.nanoTime();
                for (long i = 0; i < calls; ++i) {
                    p = sandbox.call(shift, p).to(P.class);
                }
                long t5 = System.nanoTime();
                Struct s = sandbox.call(shift, p).asStruct();
                for (long i = 0; i < calls; ++i) {
                    s = sandbox.call(shift, s).asStruct();
                }
                long t6 = System.nanoTime();
                check += p.x() + (Long) s.get("x");
                if (round == rounds - 1) {
                    double call = (double) (t1 - t0) / calls;
                    double upcall = ((double) (t3 - t2) - (t2 - t1)) / ((double) loops * inner);
                    System.out.printf(Locale.ROOT, "java call_ns=%.1f upcall_ns=%.1f struct_arg_ns=%.1f "
                            + "struct_record_ns=%.1f struct_generic_ns=%.1f check=%d%n", call, upcall,
                            (double) (t4 - t3) / calls, (double) (t5 - t4) / calls, (double) (t6 - t5) / calls,
                            check);
                }
            }
        }
    }
}
