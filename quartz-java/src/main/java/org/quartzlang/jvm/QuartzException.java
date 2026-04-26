package org.quartzlang.jvm;

import java.io.Serial;

/** Base of every error Quartz reports to Java: misuse of the library, a failed load, a compile error or a trap. */
public class QuartzException extends RuntimeException {
    @Serial
    private static final long serialVersionUID = 1L;

    public QuartzException(String message) {
        super(message);
    }

    public QuartzException(String message, Throwable cause) {
        super(message, cause);
    }
}
