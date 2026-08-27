package org.quartzlang.jvm;

/**
 * What a sandbox may spend. Zero keeps Quartz's default for that field.
 *
 * @param memoryBytes memory quota, 64 MiB by default
 * @param fuelPerCall fuel each call and each task slice gets
 * @param maxDepth recursion limit
 * @param tier which execution tier runs the code
 */
public record Limits(long memoryBytes, long fuelPerCall, int maxDepth, Tier tier) {
    public static final Limits DEFAULT = new Limits(0, 0, 0, Tier.DEFAULT);

    public enum Tier {
        DEFAULT,
        INTERPRETER,
        AUTOMATIC,
        NATIVE
    }

    public Limits {
        if (memoryBytes < 0 || fuelPerCall < 0 || maxDepth < 0) {
            throw new IllegalArgumentException("limits cannot be negative");
        }
        tier = tier == null ? Tier.DEFAULT : tier;
    }

    public Limits withMemory(long bytes) {
        return new Limits(bytes, fuelPerCall, maxDepth, tier);
    }

    public Limits withFuel(long fuel) {
        return new Limits(memoryBytes, fuel, maxDepth, tier);
    }

    public Limits withMaxDepth(int depth) {
        return new Limits(memoryBytes, fuelPerCall, depth, tier);
    }

    public Limits withTier(Tier t) {
        return new Limits(memoryBytes, fuelPerCall, maxDepth, t);
    }
}
