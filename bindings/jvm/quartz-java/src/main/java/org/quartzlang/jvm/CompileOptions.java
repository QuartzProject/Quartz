package org.quartzlang.jvm;

import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Objects;

/**
 * How to compile.
 *
 * @param emitBinary also produce the portable .qzb module ({@link Program#binary()})
 * @param keepLines keep source lines in the module for trap messages
 * @param manifest package.toml text, or null
 * @param rules warning levels by rule id ({@code unused-variable}, or {@code *} for every rule), applied in order so
 *     a later entry wins; an unknown rule id is reported as an {@code unknown-rule} warning
 */
public record CompileOptions(boolean emitBinary, boolean keepLines, String manifest, Map<String, WarningLevel> rules) {
    public static final CompileOptions DEFAULT = new CompileOptions(false, true, null);

    public CompileOptions {
        rules = rules == null ? Map.of() : Collections.unmodifiableMap(new LinkedHashMap<>(rules));
        rules.forEach((rule, level) -> {
            Objects.requireNonNull(rule, "rule");
            Objects.requireNonNull(level, "level of " + rule);
        });
    }

    public CompileOptions(boolean emitBinary, boolean keepLines, String manifest) {
        this(emitBinary, keepLines, manifest, Map.of());
    }

    public CompileOptions withBinary(boolean emit) {
        return new CompileOptions(emit, keepLines, manifest, rules);
    }

    public CompileOptions withLines(boolean keep) {
        return new CompileOptions(emitBinary, keep, manifest, rules);
    }

    public CompileOptions withManifest(String toml) {
        return new CompileOptions(emitBinary, keepLines, toml, rules);
    }

    /** Sets the level of {@code rule} ({@code *} for every rule), after and so above any level set before. */
    public CompileOptions withRule(String rule, WarningLevel level) {
        Map<String, WarningLevel> next = new LinkedHashMap<>(rules);
        next.remove(Objects.requireNonNull(rule, "rule"));
        next.put(rule, Objects.requireNonNull(level, "level"));
        return new CompileOptions(emitBinary, keepLines, manifest, next);
    }

    /** Every warning as an error, like {@code quartz check --deny-warnings}. */
    public CompileOptions denyWarnings() {
        return withRule("*", WarningLevel.ERROR);
    }
}
