package org.quartzlang.jvm;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.ArrayList;
import java.util.List;
import org.junit.jupiter.api.Test;

class IntrospectionTest {
    @Test
    void diagnosticsCarryRulesAndRuleLevelsDecideFailure() {
        Source lint = Source.of("lint.qz", "fn f(x: int) -> int {\n    let y = 1\n    return x\n}\n");
        try (Program program = Quartz.compile(List.of(lint), CompileOptions.DEFAULT)) {
            assertEquals(List.of(new Diagnostic(Diagnostic.Severity.WARNING, "unused-variable", "lint.qz", 2, 9, 2, 10,
                    program.diagnostics().get(0).message())), program.diagnostics());
            assertTrue(program.diagnosticsText().contains("[unused-variable]"));
        }
        CompileException denied = assertThrows(CompileException.class, () -> Quartz.compile(List.of(lint),
                CompileOptions.DEFAULT.denyWarnings().withRule("unused-parameter", WarningLevel.IGNORE)));
        assertEquals("unused-variable", denied.errors().get(0).rule());
        try (Program quiet = Quartz.compile(List.of(lint),
                CompileOptions.DEFAULT.withRule("unused-variable", WarningLevel.IGNORE))) {
            assertTrue(quiet.diagnostics().isEmpty());
        }
        assertTrue(Quartz.warningRules().contains("unused-variable"));
    }

    @Test
    void exportsImportsTypesAndAttributesDescribeTheProgram() {
        String host = "package org.example.host\n@handle\nstruct Player {}\n"
                + "impl Player {\n    fn level(self) -> int\n}\nfn unused(n: int)\n";
        String plugin = """
                package org.example.a
                import org.example.host.Player
                @command("greet", perm = "x.greet", times = 2)
                pub fn greet(p: Player, times: int) -> int {
                    return p.level() * times
                }
                @on(Join, where = p.level() > 3)
                fn joined(p: Player) {}
                suspend fn later() {}
                """;
        try (Program program = Quartz.compile(Source.of("host.qzi", host), Source.of("a.qz", plugin))) {
            Export greet = program.export("greet");
            assertEquals("org.example.a.greet", greet.name());
            assertEquals("greet", greet.simpleName());
            assertEquals(List.of("p", "times"), greet.parameterNames());
            assertEquals(Visibility.PUBLIC, greet.visibility());
            Type player = greet.signature().params().get(0);
            assertEquals("org.example.host.Player", player.qualifiedName());
            Attribute command = greet.attribute("command");
            assertEquals("greet", command.positional(0).orElseThrow().asString());
            assertEquals(2, command.named("times").orElseThrow().asLong());
            Attribute on = program.export("joined").attribute("on");
            assertEquals(Attribute.ArgumentKind.NAME, on.positional(0).orElseThrow().kind());
            assertNotNull(on.filter());
            assertTrue(program.export("later").isSuspend());
            List<String> names = new ArrayList<>();
            program.exports().forEach(e -> names.add(e.simpleName()));
            assertTrue(names.containsAll(List.of("greet", "joined", "later")), names.toString());
            assertSame(greet, program.exports().get(names.indexOf("greet")));
            Import level = program.findImport("Player.level").orElseThrow();
            assertEquals("org.example.host", level.packageName());
            assertTrue(level.required());
            assertFalse(program.findImport("unused").orElseThrow().required());
        }
    }
}
