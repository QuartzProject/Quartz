package org.quartzlang.jvm;

import static java.lang.foreign.ValueLayout.JAVA_LONG;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** Builds and reads the lists and maps of one sandbox through the C interface, element by element. */
final class Heap {
    private Heap() {}

    static List<Object> readList(Type type, long bits, Slots.Readers readers) {
        Type e = type.element();
        int width = e.slots();
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment cell = arena.allocate(JAVA_LONG, Math.max(1, width));
            int n = Math.toIntExact((long) Native.LIST_LENGTH.invokeExact(bits));
            List<Object> out = new ArrayList<>(n);
            long[] slots = new long[Math.max(1, width)];
            for (int i = 0; i < n; ++i) {
                if (!(boolean) Native.LIST_GET.invokeExact(bits, (long) i, cell, (long) width)) {
                    throw new QuartzException("cannot read element " + i + " of a " + type);
                }
                if (width == 1 && !Slots.isComposite(e.kind())) {
                    out.add(Slots.box(e, cell.get(JAVA_LONG, 0), readers));
                } else {
                    MemorySegment.copy(cell, JAVA_LONG, 0, slots, 0, width);
                    out.add(Slots.decode(e, slots, 0, readers));
                }
            }
            return out;
        } catch (Throwable t) {
            throw Native.rethrow(t);
        }
    }

    static Map<Object, Object> readMap(MemorySegment sandbox, Type type, long bits, Slots.Readers readers) {
        Type k = type.key();
        Type v = type.element();
        int keyWidth = Math.max(1, k.slots());
        int valueWidth = Math.max(1, v.slots());
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment key = arena.allocate(JAVA_LONG, keyWidth);
            MemorySegment value = arena.allocate(JAVA_LONG, valueWidth);
            long[] keySlots = new long[keyWidth];
            long[] valueSlots = new long[valueWidth];
            long n = (long) Native.MAP_LENGTH.invokeExact(bits);
            Map<Object, Object> out = new LinkedHashMap<>();
            for (long i = 0; i < n; ++i) {
                if (!(boolean) Native.MAP_ENTRY.invokeExact(sandbox, bits, i, key, value)) {
                    throw new QuartzException("cannot read entry " + i + " of a " + type);
                }
                MemorySegment.copy(key, JAVA_LONG, 0, keySlots, 0, keyWidth);
                MemorySegment.copy(value, JAVA_LONG, 0, valueSlots, 0, valueWidth);
                out.put(Slots.decode(k, keySlots, 0, readers), Slots.decode(v, valueSlots, 0, readers));
            }
            return out;
        } catch (Throwable t) {
            throw Native.rethrow(t);
        }
    }

    static long newList(MemorySegment sandbox, Sandbox owner, Type type, List<?> elements, Slots.Makers makers)
            throws Throwable {
        Type e = type.element();
        int width = e.slots();
        long list = (long) Native.SANDBOX_LIST.invokeExact(sandbox, type.id());
        if (list == 0) {
            throw new TrapException(TrapCode.OUT_OF_MEMORY, "cannot make a " + type);
        }
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment cell = arena.allocate(JAVA_LONG, Math.max(1, width));
            long[] slots = new long[Math.max(1, width)];
            for (Object x : elements) {
                Slots.encode(e, x, slots, 0, owner, makers);
                MemorySegment.copy(slots, 0, cell, JAVA_LONG, 0, width);
                if (!(boolean) Native.LIST_PUSH.invokeExact(sandbox, list, cell, (long) width)) {
                    throw new TrapException(TrapCode.OUT_OF_MEMORY, "cannot grow the list");
                }
            }
        }
        return list;
    }

    static long newMap(MemorySegment sandbox, Sandbox owner, Type type, Map<?, ?> entries, Slots.Makers makers)
            throws Throwable {
        Type k = type.key();
        Type v = type.element();
        long map = (long) Native.SANDBOX_MAP.invokeExact(sandbox, type.id());
        if (map == 0) {
            throw new TrapException(TrapCode.OUT_OF_MEMORY, "cannot make a " + type);
        }
        int keyWidth = Math.max(1, k.slots());
        int valueWidth = Math.max(1, v.slots());
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment key = arena.allocate(JAVA_LONG, keyWidth);
            MemorySegment value = arena.allocate(JAVA_LONG, valueWidth);
            long[] keySlots = new long[keyWidth];
            long[] valueSlots = new long[valueWidth];
            for (Map.Entry<?, ?> entry : entries.entrySet()) {
                Slots.encode(k, entry.getKey(), keySlots, 0, owner, makers);
                Slots.encode(v, entry.getValue(), valueSlots, 0, owner, makers);
                MemorySegment.copy(keySlots, 0, key, JAVA_LONG, 0, keyWidth);
                MemorySegment.copy(valueSlots, 0, value, JAVA_LONG, 0, valueWidth);
                if (!(boolean) Native.MAP_SET.invokeExact(sandbox, map, key, value)) {
                    throw new TrapException(TrapCode.OUT_OF_MEMORY, "cannot grow the map");
                }
            }
        }
        return map;
    }
}
