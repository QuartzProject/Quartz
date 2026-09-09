package org.quartzlang.jvm;

import java.io.Serial;

/** A Quartz {@code Err} turned into an exception by {@link Result#orElseThrow()}; not a trap. */
public final class ResultException extends QuartzException {
    @Serial
    private static final long serialVersionUID = 1L;

    private final transient Object error;

    public ResultException(Object error) {
        super(String.valueOf(error));
        this.error = error;
    }

    /** What the {@code Err} carried. */
    public Object error() {
        return error;
    }
}
