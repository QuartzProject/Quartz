package org.quartzlang.jvm.kotlin

import kotlin.test.Test
import kotlin.test.assertEquals

class RecursiveKtTest {
    sealed interface Expr {
        data class Num(val value: Long) : Expr

        data class Add(val left: Expr, val right: Expr) : Expr

        data class Neg(val inner: Expr) : Expr
    }

    @Test
    fun recursiveEnumsReadAsSealedClassesAndCarryDocs() {
        val program = compileQuartz(
            "expr.qz" to """
                /// An arithmetic expression.
                enum Expr {
                    Num(int)
                    Add(Expr, Expr)
                    /// Negation.
                    Neg(Expr)
                }
                /// Evaluates an expression.
                fn eval(e: Expr) -> int {
                    return match e {
                        Expr.Num(n) => n,
                        Expr.Add(a, b) => eval(a) + eval(b),
                        Expr.Neg(x) => -eval(x),
                    }
                }
                fn simplify(e: Expr) -> Expr {
                    return match e {
                        Expr.Neg(inner) => match inner {
                            Expr.Neg(x) => simplify(x),
                            _ => Expr.Neg(simplify(inner)),
                        },
                        Expr.Add(a, b) => Expr.Add(simplify(a), simplify(b)),
                        _ => e,
                    }
                }
            """.trimIndent(),
        )
        val e = Expr.Add(Expr.Num(2), Expr.Neg(Expr.Neg(Expr.Num(5))))
        program.withSandbox { sandbox ->
            sandbox.start()
            assertEquals(7L, sandbox.callAs<Long>("eval", e))
            assertEquals(Expr.Add(Expr.Num(2), Expr.Num(5)), sandbox.callAs<Expr>("simplify", e))
        }
        val eval = program.export("eval")
        assertEquals("Evaluates an expression.", eval.doc())
        val type = eval.signature().params()[0]
        assertEquals("An arithmetic expression.", type.doc())
        assertEquals("Negation.", type.findCase("Neg").doc())
    }
}
