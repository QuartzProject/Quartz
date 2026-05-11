package org.quartzlang.kmp

/** Base of every error Quartz reports: misuse, a failed load, a compile error or a trap. */
public open class QuartzException(message: String?, cause: Throwable? = null) : RuntimeException(message, cause)

/** Compilation failed; [diagnostics] holds every error and warning as the compiler printed them. */
public class CompileException(public val diagnostics: String) :
    QuartzException(diagnostics.trim().ifEmpty { "compilation failed" })

/** The program trapped; the sandbox stays usable. When a host function threw, that is the [cause]. */
public class TrapException(public val code: TrapCode, message: String?, cause: Throwable? = null) :
    QuartzException(message, cause)
