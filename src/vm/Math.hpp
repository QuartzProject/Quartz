#pragma once

namespace quartz::vm::math {

// Deterministic on every platform (D57): only correctly rounded IEEE operations in a fixed order, no libm.
double sin(double x);
double cos(double x);
double pow(double x, double y);

} // namespace quartz::vm::math
