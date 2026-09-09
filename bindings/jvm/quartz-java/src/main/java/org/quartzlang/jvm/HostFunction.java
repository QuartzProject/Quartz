package org.quartzlang.jvm;

/** A host import: returns what {@link Sandbox#call} accepts for its result type; anything it throws is a trap. */
@FunctionalInterface
public interface HostFunction {
    Object call(HostCall call) throws Exception;
}
