package org.quartzlang.jvm;

/** How {@link Quartz#deflate} wraps DEFLATE data: bare (RFC 1951), zlib (RFC 1950) or gzip (RFC 1952). */
public enum Framing {
    RAW,
    ZLIB,
    GZIP
}
