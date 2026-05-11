package org.quartzlang.jvm;

import java.io.Serial;

/** The program trapped (fuel, memory, panic, division by zero, a failed host function); the sandbox stays usable. */
public final class TrapException extends QuartzException {
    @Serial
    private static final long serialVersionUID = 1L;

    private final TrapCode code;

    public TrapException(TrapCode code, String message) {
        this(code, message, null);
    }

    public TrapException(TrapCode code, String message, Throwable cause) {
        super(message, cause);
        this.code = code == null ? TrapCode.PANIC : code;
    }

    /** What kind of trap it was; {@link TrapCode#HOST_ERROR} when a host function failed. */
    public TrapCode code() {
        return code;
    }
}
