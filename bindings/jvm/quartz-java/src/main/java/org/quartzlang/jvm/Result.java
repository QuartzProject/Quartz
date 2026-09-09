package org.quartzlang.jvm;

import java.util.function.Function;

/**
 * A Quartz {@code Result<T, E>}: a host function returns {@link #ok} or {@link #err}, and a result reads as one.
 *
 * @param <T> what success carries
 * @param <E> what failure carries
 */
public sealed interface Result<T, E> {
    record Ok<T, E>(T value) implements Result<T, E> {}

    record Err<T, E>(E error) implements Result<T, E> {}

    static <T, E> Result<T, E> ok(T value) {
        return new Ok<>(value);
    }

    static <T, E> Result<T, E> err(E error) {
        return new Err<>(error);
    }

    default boolean isOk() {
        return this instanceof Ok;
    }

    /** The success value, or {@link ResultException} carrying the error. */
    default T orElseThrow() {
        return switch (this) {
            case Ok<T, E> ok -> ok.value();
            case Err<T, E> err -> throw new ResultException(err.error());
        };
    }

    default <U> Result<U, E> map(Function<? super T, ? extends U> f) {
        return switch (this) {
            case Ok<T, E> ok -> new Ok<>(f.apply(ok.value()));
            case Err<T, E> err -> new Err<>(err.error());
        };
    }
}
