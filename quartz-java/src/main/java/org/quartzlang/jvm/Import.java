package org.quartzlang.jvm;

/**
 * A host function a program imports.
 *
 * @param name the name to bind it by ({@code log}, {@code Player.health})
 * @param signature what it takes and returns
 */
public record Import(String name, Signature signature) {}
