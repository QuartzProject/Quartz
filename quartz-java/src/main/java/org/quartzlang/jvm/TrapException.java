package org.quartzlang.jvm;

import java.io.Serial;

/** The program trapped (fuel, memory, panic, division by zero, a failed host function); the sandbox stays usable. */
public final class TrapException extends QuartzException {
    @Serial
    private static final long serialVersionUID = 1L;

    public TrapException(String message) {
        super(message);
    }

    public TrapException(String message, Throwable cause) {
        super(message, cause);
    }
}
