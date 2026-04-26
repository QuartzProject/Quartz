package org.quartzlang.jvm;

import java.lang.invoke.MethodHandles;
import java.lang.invoke.VarHandle;
import java.lang.ref.WeakReference;
import java.util.ArrayList;
import java.util.List;

/** The one thread that may use a group of linked sandboxes now; it may nest calls into other members. */
final class Confinement {
    private static final VarHandle OWNER;

    static {
        try {
            OWNER = MethodHandles.lookup().findVarHandle(Confinement.class, "owner", Thread.class);
        } catch (ReflectiveOperationException e) {
            throw new ExceptionInInitializerError(e);
        }
    }

    @SuppressWarnings("unused")
    private volatile Thread owner;
    private int depth;
    /** Bumped whenever any member may reset the arena that holds earlier results. */
    long generation;
    /** What a host function threw, kept as the cause of the trap it becomes. */
    Throwable hostFailure;
    private final List<WeakReference<Sandbox>> members = new ArrayList<>();

    Confinement(Sandbox first) {
        members.add(new WeakReference<>(first));
    }

    static Confinement enter(Sandbox sandbox) {
        Thread me = Thread.currentThread();
        while (true) {
            Confinement c = sandbox.confinement;
            if (c.owner == me) {
                c.depth++;
                return c;
            }
            if (!OWNER.compareAndSet(c, null, me)) {
                throw new IllegalStateException("sandbox is in use by another thread");
            }
            if (c != sandbox.confinement) {
                OWNER.setRelease(c, null);
                continue;
            }
            c.depth = 1;
            return c;
        }
    }

    void exit() {
        if (--depth == 0) {
            OWNER.setRelease(this, null);
        }
    }

    boolean nested() {
        return depth > 1;
    }

    /** Moves every member of {@code other} into this group; the caller owns both, outside any call. */
    void absorb(Confinement other) {
        if (other == this) {
            return;
        }
        generation = Math.max(generation, other.generation) + 1;
        for (WeakReference<Sandbox> ref : other.members) {
            Sandbox s = ref.get();
            if (s != null) {
                s.confinement = this;
                members.add(ref);
            }
        }
        other.members.clear();
        other.depth = 0;
        OWNER.setRelease(other, null);
    }
}
