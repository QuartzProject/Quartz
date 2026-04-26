package org.quartzlang.jvm;

import java.lang.foreign.MemorySegment;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Consumer;

/** A native object freed exactly once, when its owner and everything that depends on it have let go. */
final class NativeHandle {
    private final MemorySegment address;
    private final Consumer<MemorySegment> free;
    private final AtomicInteger refs = new AtomicInteger(1);
    private final List<NativeHandle> dependencies = new ArrayList<>();
    private final List<Runnable> afterFree = new ArrayList<>();

    NativeHandle(MemorySegment address, Consumer<MemorySegment> free) {
        this.address = address;
        this.free = free;
    }

    MemorySegment address() {
        return address;
    }

    boolean alive() {
        return refs.get() > 0;
    }

    /** Keeps {@code other} alive until this handle is freed. */
    synchronized void dependOn(NativeHandle other) {
        other.retain();
        dependencies.add(other);
    }

    synchronized void afterFree(Runnable action) {
        afterFree.add(action);
    }

    void retain() {
        while (true) {
            int n = refs.get();
            if (n == 0) {
                throw new IllegalStateException("native object already freed");
            }
            if (refs.compareAndSet(n, n + 1)) {
                return;
            }
        }
    }

    void release() {
        if (refs.decrementAndGet() != 0) {
            return;
        }
        free.accept(address);
        List<NativeHandle> deps;
        List<Runnable> actions;
        synchronized (this) {
            deps = List.copyOf(dependencies);
            actions = List.copyOf(afterFree);
        }
        actions.forEach(Runnable::run);
        deps.forEach(NativeHandle::release);
    }
}
