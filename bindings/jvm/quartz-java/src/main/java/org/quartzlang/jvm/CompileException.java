package org.quartzlang.jvm;

import java.io.Serial;

/** Compilation failed; {@link #diagnostics()} holds every error and warning as the compiler printed them. */
public final class CompileException extends QuartzException {
    @Serial
    private static final long serialVersionUID = 1L;

    private final String diagnostics;

    public CompileException(String diagnostics) {
        super(diagnostics == null || diagnostics.isBlank() ? "compilation failed" : diagnostics.strip());
        this.diagnostics = diagnostics == null ? "" : diagnostics;
    }

    public String diagnostics() {
        return diagnostics;
    }
}
