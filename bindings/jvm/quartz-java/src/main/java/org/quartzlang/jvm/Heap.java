package org.quartzlang.jvm;

import java.lang.foreign.MemorySegment;

/** Builds and reads the lists and maps of one sandbox through the C interface, a level at a time. */
final class Heap {
    private Heap() {}

    static int listLength(long bits) {
        try {
            return Math.toIntExact((long) Native.LIST_LENGTH.invokeExact(bits));
        } catch (Throwable t) {
            throw Native.rethrow(t);
        }
    }

    static void listGet(Type type, long bits, int index, long[] out) {
        try {
            long width = type.element().slots();
            if (!(boolean) Native.LIST_GET.invokeExact(bits, (long) index, MemorySegment.ofArray(out), width)) {
                throw new QuartzException("cannot read element " + index + " of a " + type);
            }
        } catch (Throwable t) {
            throw Native.rethrow(t);
        }
    }

    static long mapLength(long bits) {
        try {
            return (long) Native.MAP_LENGTH.invokeExact(bits);
        } catch (Throwable t) {
            throw Native.rethrow(t);
        }
    }

    static void mapEntry(MemorySegment sandbox, Type type, long bits, long index, long[] key, long[] value) {
        try {
            if (!(boolean) Native.MAP_ENTRY.invokeExact(sandbox, bits, index, MemorySegment.ofArray(key),
                    MemorySegment.ofArray(value))) {
                throw new QuartzException("cannot read entry " + index + " of a " + type);
            }
        } catch (Throwable t) {
            throw Native.rethrow(t);
        }
    }

    static long newList(MemorySegment sandbox, Type type, long[] slots, int count) throws Throwable {
        long width = type.element().slots();
        long list = (long) Native.SANDBOX_LIST.invokeExact(sandbox, type.id());
        if (list == 0) {
            throw new TrapException(TrapCode.OUT_OF_MEMORY, "cannot make a " + type);
        }
        MemorySegment all = MemorySegment.ofArray(slots);
        for (int i = 0; i < count; ++i) {
            if (!(boolean) Native.LIST_PUSH.invokeExact(sandbox, list, all.asSlice(i * width * Long.BYTES), width)) {
                throw new TrapException(TrapCode.OUT_OF_MEMORY, "cannot grow the list");
            }
        }
        return list;
    }

    static long newMap(MemorySegment sandbox, Type type, long[] keys, long[] values, int count) throws Throwable {
        long map = (long) Native.SANDBOX_MAP.invokeExact(sandbox, type.id());
        if (map == 0) {
            throw new TrapException(TrapCode.OUT_OF_MEMORY, "cannot make a " + type);
        }
        long keyBytes = (long) type.key().slots() * Long.BYTES;
        long valueBytes = (long) type.element().slots() * Long.BYTES;
        MemorySegment k = MemorySegment.ofArray(keys);
        MemorySegment v = MemorySegment.ofArray(values);
        for (int i = 0; i < count; ++i) {
            if (!(boolean) Native.MAP_SET.invokeExact(sandbox, map, k.asSlice(i * keyBytes),
                    v.asSlice(i * valueBytes))) {
                throw new TrapException(TrapCode.OUT_OF_MEMORY, "cannot grow the map");
            }
        }
        return map;
    }
}
