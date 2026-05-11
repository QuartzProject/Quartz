package org.quartzlang.jvm;

import java.util.List;

/**
 * Parameters a caller passes and the result of a function or host import ({@code state} parameters are left out).
 *
 * @param params one type per parameter
 * @param result the result type, {@link Kind#VOID} when there is none
 */
public record Signature(List<Type> params, Type result) {
    public Signature {
        params = List.copyOf(params);
    }

    public int arity() {
        return params.size();
    }

    /** Slot offset of parameter {@code index} in the argument array. */
    int offset(int index) {
        int at = 0;
        for (int i = 0; i < index; ++i) {
            at += params.get(i).slots();
        }
        return at;
    }

    @Override
    public String toString() {
        return params + " -> " + result;
    }
}
