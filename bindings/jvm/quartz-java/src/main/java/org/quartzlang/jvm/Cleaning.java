package org.quartzlang.jvm;

import java.lang.ref.Cleaner;

final class Cleaning {
    static final Cleaner CLEANER = Cleaner.create(Thread.ofPlatform().daemon().name("quartz-cleaner").factory());

    private Cleaning() {}
}
