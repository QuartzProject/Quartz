package org.quartzlang.jvm;

import java.io.Serial;
import java.util.List;

/** Compilation failed; {@link #diagnostics()} holds every error and warning, {@link #text()} them as printed. */
public final class CompileException extends QuartzException {
    @Serial
    private static final long serialVersionUID = 2L;

    private final String text;
    private final transient List<Diagnostic> diagnostics;

    public CompileException(String text) {
        this(text, List.of());
    }

    public CompileException(String text, List<Diagnostic> diagnostics) {
        super(text == null || text.isBlank() ? "compilation failed" : text.strip());
        this.text = text == null ? "" : text;
        this.diagnostics = List.copyOf(diagnostics);
    }

    /** Every diagnostic, errors and warnings, in the order the compiler reported them. */
    public List<Diagnostic> diagnostics() {
        return diagnostics == null ? List.of() : diagnostics;
    }

    /** Only the errors. */
    public List<Diagnostic> errors() {
        return diagnostics().stream().filter(Diagnostic::isError).toList();
    }

    /** The diagnostics rendered as the compiler prints them. */
    public String text() {
        return text;
    }
}
