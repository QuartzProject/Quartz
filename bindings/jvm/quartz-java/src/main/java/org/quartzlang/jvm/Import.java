package org.quartzlang.jvm;

/**
 * A host function a program imports.
 *
 * @param name the name to bind it by, with its package ({@code org.example.host.greet}, {@code Player.health})
 * @param signature what it takes and returns
 * @param builtIn true when the runtime binds it itself (the standard library); the host binds the others
 * @param packageName the package of the interface that declares it; empty when it has none
 * @param required false when no function calls it, so {@link Sandbox#start()} does not ask for its binding
 */
public record Import(String name, Signature signature, boolean builtIn, String packageName, boolean required) {
    public Import {
        packageName = packageName == null ? "" : packageName;
    }

    public Import(String name, Signature signature, boolean builtIn) {
        this(name, signature, builtIn, "", true);
    }

    /** The name without its package ({@code greet}, {@code Player.health}). */
    public String simpleName() {
        return packageName.isEmpty() || !name.startsWith(packageName + ".") ? name
                : name.substring(packageName.length() + 1);
    }
}
