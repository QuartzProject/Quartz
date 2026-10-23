package org.quartzlang.jvm;

import java.util.Locale;
import java.util.Objects;

/**
 * One compiler diagnostic. Lines and columns start at 1 and columns count UTF-8 bytes; all four are 0 when the
 * diagnostic has no place in a source.
 *
 * @param severity error, warning or note
 * @param rule the warning rule id ({@code unused-variable}), kept when the warning was raised to an error; empty for
 *     other errors and for notes
 * @param file the source name as it was given to the compiler; empty when there is none
 * @param line first line
 * @param column first column
 * @param endLine last line
 * @param endColumn one past the last column
 * @param message the text, ending in {@code [rule]} for a rule's warning
 */
public record Diagnostic(Severity severity, String rule, String file, int line, int column, int endLine,
        int endColumn, String message) {
    public enum Severity {
        ERROR,
        WARNING,
        NOTE;

        static Severity of(int code) {
            return code >= 0 && code < 3 ? values()[code] : ERROR;
        }
    }

    public Diagnostic {
        Objects.requireNonNull(severity, "severity");
        rule = rule == null ? "" : rule;
        file = file == null ? "" : file;
        message = message == null ? "" : message;
    }

    public boolean isError() {
        return severity == Severity.ERROR;
    }

    /** {@code file:line:column: severity: message}, the shape editors turn into links. */
    @Override
    public String toString() {
        String where = file.isEmpty() ? "" : line == 0 ? file + ": " : file + ":" + line + ":" + column + ": ";
        return where + severity.name().toLowerCase(Locale.ROOT) + ": " + message;
    }
}
