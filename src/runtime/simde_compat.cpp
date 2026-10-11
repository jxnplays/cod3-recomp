// =============================================================================
//  COD3 Recomp - SIMDE compatibility shims
//
//  simde emits calls to a handful of libm-style helpers that MSVC's UCRT does
//  not export on every toolchain version. The Xbox 360's PowerPC has a
//  round-to-nearest-even instruction (used by the vector float instructions the
//  title uses heavily), which simde lowers to `roundeven`/`roundevenf`.
//
//  These are the standard C names with C linkage, so defining them here resolves
//  the link. The semantics required are "round to nearest, ties to even", which
//  is exactly what the default floating-point rounding mode does on x86, so
//  `std::nearbyint` is the correct and cheapest implementation.
//
//  If a future toolchain starts exporting these from the CRT, this file can be
//  deleted without any other change.
// =============================================================================

#include <cmath>

extern "C" {

float roundevenf(float x)
{
    // nearbyint honours the current rounding mode, which is "to nearest, ties
    // to even" on x86 unless something has changed it. That is precisely the
    // PowerPC behaviour being emulated.
    return std::nearbyintf(x);
}

double roundeven(double x)
{
    return std::nearbyint(x);
}

} // extern "C"