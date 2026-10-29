package org.quartzlang.jvm;

import java.util.Arrays;
import java.util.Objects;

/**
 * Bytes passed where a {@code str} goes, decoded with each ill-formed UTF-8 sequence as U+FFFD, so text from outside
 * (process output, headers) never fails for its content; valid anywhere a String is, at any depth.
 *
 * @param bytes the text, UTF-8 or close to it
 */
public record LossyText(byte[] bytes) {
    public LossyText {
        bytes = Objects.requireNonNull(bytes, "bytes").clone();
    }

    @Override
    public byte[] bytes() {
        return bytes.clone();
    }

    byte[] raw() {
        return bytes;
    }

    @Override
    public boolean equals(Object o) {
        return o instanceof LossyText t && Arrays.equals(t.bytes, bytes);
    }

    @Override
    public int hashCode() {
        return Arrays.hashCode(bytes);
    }

    @Override
    public String toString() {
        return "LossyText[" + bytes.length + " bytes]";
    }
}
