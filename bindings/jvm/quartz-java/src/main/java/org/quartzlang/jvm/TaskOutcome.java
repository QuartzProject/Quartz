package org.quartzlang.jvm;

/**
 * What a task did before handing control back.
 *
 * @param kind why it stopped
 * @param task the task id to resume or cancel
 * @param value DONE: the whole result, as {@link Sandbox#call} gives it; WAIT and AT_*: the awaited handle
 */
public record TaskOutcome(Kind kind, int task, Value value) {
    public enum Kind {
        DONE,
        WAIT,
        AT_ENTER,
        AT_LEAVE,
        YIELD
    }

    public boolean done() {
        return kind == Kind.DONE;
    }
}
