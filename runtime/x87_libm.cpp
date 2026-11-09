// runtime/x87_libm.cpp — the long double libm functions the lifted x87 semantics call, with the ABI they were built
// for: a double in, a double out.
//
// Remill's X87 semantics (FSQRT, FPATAN, FPTAN, FYL2X, F2XM1, FPREM, FPREM1) compute on its native_float80_t, which
// is a double off x86, and call sqrtl, atanl, tanl, log2l, exp2l, fmodl and remainderl. Its semantics bitcode was
// built on macOS arm64, where long double is a double too, so the calls pass and expect a double (d0). On Android and
// glibc arm64 a long double is a 128-bit quad, passed in q0: libm's sqrtl read a quad made of the double's bits and
// answered a quad, and the game read its low half. Measured in the harness: Lua math.sqrt(10961) = -inf,
// sqrt(17146) = -0, sqrt(4.41e6) = 3.4e14. scripts/package-game.sh renames the calls in lifted.o to these.
#include <cmath>

extern "C" {
double bo2_x87_sqrtl(double x) { return std::sqrt(x); }
double bo2_x87_atanl(double x) { return std::atan(x); }
double bo2_x87_tanl(double x) { return std::tan(x); }
double bo2_x87_log2l(double x) { return std::log2(x); }
double bo2_x87_exp2l(double x) { return std::exp2(x); }
double bo2_x87_fmodl(double x, double y) { return std::fmod(x, y); }
double bo2_x87_remainderl(double x, double y) { return std::remainder(x, y); }
}
