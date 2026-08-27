package org.quartzlang.jvm;

/** A host import: returns a boxed scalar, String, byte[] or null for void; anything it throws becomes a trap. */
@FunctionalInterface
public interface HostFunction {
    Object call(HostCall call) throws Exception;
}
