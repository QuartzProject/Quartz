package org.quartzlang.jvm;

/**
 * A host function a program imports.
 *
 * @param name the name to bind it by ({@code log}, {@code Player.health})
 * @param signature what it takes and returns
 * @param builtIn true when the runtime binds it itself (the standard library); the host binds the others
 */
public record Import(String name, Signature signature, boolean builtIn) {}
