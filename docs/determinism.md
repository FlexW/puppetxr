# Determinism

A replayed recording must produce the same poses, times, action states and
screenshots as the original run, on any platform. Everything below exists for that.

## Rules

1. The only wall-clock read is `SIM_Clock` in realtime mode. Replay, headless and
   CI use fixed-step mode, where time advances by a configured period per `xrWaitFrame`.
   Exceptions that change nothing the simulator computes: the frame gate's timeout
   (`docs/openxr.md`, Frame loop), which only ends an app's deadlock; the profiler
   (`docs/profiling.md`); the mirror window's present rate and the Performance tab's frame
   times (`docs/compositor.md`), which are only shown.
2. Device updates and event processing happen in one fixed order, once per
   `xrWaitFrame`. Nothing in the runtime depends on when the app calls other functions
   within a frame.
3. All floating point math goes through `zmath`. No `<math.h>` outside `tests/`.
4. Build flags: `-ffp-contract=off` (no FMA), no `-ffast-math`, `-fno-math-errno`.
   `PUPPETXR_FAST_MATH=ON` relaxes this and is unsupported for recordings.
5. Randomness, if ever needed, takes a seed from config and uses our own generator.

## zmath transcendentals

`sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `exp`, `log`, `pow` are our own
code in `src/zmath.cpp`. They evaluate in f64 with plain IEEE add, multiply and divide,
then round once to f32, so every platform with IEEE-754 doubles and no FMA
contraction produces identical bits. Properties:

| Function | Method | Accuracy vs libm | Notes |
|---|---|---|---|
| sin, cos, tan | Cody-Waite reduction by pi/2 (3-part constant), Taylor to degree 15/16 | < 2e-6 on [-100, 100] | reduction exact for \|x\| < 1.6e6 |
| atan | argument halving to [0, tan(pi/12)], Taylor to degree 27 | < 2e-6 | |
| atan2 | atan plus IEEE quadrant and signed-zero rules | < 2e-6 | matches libm for zeros and infinities |
| asin, acos | atan2 with hardware sqrt | < 4e-6 | NaN outside [-1, 1] |
| exp | reduction by ln2 (2-part constant), Taylor to degree 13, scale by exponent bits | < 2e-6 relative | |
| log | mantissa in [sqrt(1/2), sqrt(2)], series in (m-1)/(m+1) to degree 21 | < 2e-6 relative | subnormals handled |
| pow | exp(y log x), integer-exponent sign for negative bases | < 4e-6 relative | |
| sqrt | hardware instruction (IEEE correctly rounded) | exact | `-fno-math-errno` keeps it inline |

`floor`, `ceil`, `round`, `trunc` and `fmod` go through i64 conversion, no libm.

## Tests

`tests/tests_zmath.cpp`:
- `transcendentals_match_libm_within_tolerance` checks accuracy against the host libm.
- `transcendentals_are_bit_identical_to_golden` hashes a sweep of outputs and compares
  with constants captured from the x86-64 reference build. The same constants were
  verified with gcc Debug, gcc Release and clang Release. A change here means
  recordings made with the previous build no longer replay identically: bump the
  recording format version when that is intended.
- `no_fused_multiply_add` fails if the compiler contracts `a * b - c`.
