#pragma once

namespace quartz::vm::math {

// Deterministic on every platform (D57): only correctly rounded IEEE operations in a fixed order, no libm.
double sin(double x);
double cos(double x);
double pow(double x, double y);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double exp(double x);
double log(double x);
double log10(double x);
/// Halfway cases away from zero.
double round(double x);

using UnaryFn = double (*)(double);
/// Indexed by Intrinsic::MathTan .. Intrinsic::MathRound, for the interpreter's MathF1.
extern const UnaryFn kUnary[8];

} // namespace quartz::vm::math
