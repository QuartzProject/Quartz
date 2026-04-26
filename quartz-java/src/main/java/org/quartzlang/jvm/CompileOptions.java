package org.quartzlang.jvm;

/**
 * How to compile.
 *
 * @param emitBinary also produce the portable .qzb module ({@link Program#binary()})
 * @param keepLines keep source lines in the module for trap messages
 * @param manifest package.toml text, or null
 */
public record CompileOptions(boolean emitBinary, boolean keepLines, String manifest) {
    public static final CompileOptions DEFAULT = new CompileOptions(false, true, null);

    public CompileOptions withBinary(boolean emit) {
        return new CompileOptions(emit, keepLines, manifest);
    }

    public CompileOptions withLines(boolean keep) {
        return new CompileOptions(emitBinary, keep, manifest);
    }

    public CompileOptions withManifest(String toml) {
        return new CompileOptions(emitBinary, keepLines, toml);
    }
}
