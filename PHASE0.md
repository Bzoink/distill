# Phase 0 result

Date: 2026-09-14

Phase 0 is a headless proof of concept. It has no UI or renderer. The build now contains three runnable comparison targets:

- `build/distill` — the C23 application target, QuickJS-ng with the linkedom-compatible host surface and Lexbor extraction.
- `build/distill-arm-b` — Arm B, QuickJS-ng with the self-contained linkedom JS-side DOM and current host bridges.
- `build/distill-arm-b-pruned` — Arm B′, the no-Lexbor configuration. It runs the same linkedom arm, serializes the result, and feeds that serialized HTML through litehtml. `ldd` shows QuickJS-ng and libcurl, with no Lexbor; litehtml is statically linked.

## Reproduction

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
python3 corpus/check.py --baseline
python3 corpus/check.py --arm a
python3 corpus/check.py --arm b
python3 corpus/check.py --arm b-pruned
```

The reference-browser baseline and all three engine configurations pass the local DOM assertions. The browser-only divergence for `03b-hang` is expected: Chromium times out while the engines cap at 5 seconds and dump the DOM. `05-geometry` and `08-messy` likewise verify the intentional engine-vs-browser behavior described in the manifest.

## Measurements

| Criterion | Arm A | Arm B | Arm B′ |
|---|---:|---:|---:|
| Local DOM gate assertions | 100% | 100% | 100% |
| Typical settle fixture wall time | 0.82 s | 0.8 s | 0.8 s |
| Peak RSS sample | 14,412 KiB | 15,044 KiB | comparable to Arm B |
| Linked shared footprint | 5,178,368 bytes | 1,387,480 bytes | 1,387,480 bytes |
| 03b hard-cap outcome | `cap_hit=true` | `cap_hit=true` | `cap_hit=true` |

The RSS sample was taken while running the settle-cap fixture and is a smoke measurement, not a production benchmark. All measured values are comfortably below the provisional Phase 0 budgets. Readability's pinned upstream checkout also passes its upstream test suite: 1,984 tests.

Arm A's telemetry is enabled by default on stderr and through `--telemetry FILE`. Geometry calls are explicitly flagged; the geometry fixture records IntersectionObserver, getBoundingClientRect, offsetHeight, offsetWidth, and scroll calls. Unimplemented DOM members return benign values through the host fallback path and are intended to grow the same log as the corpus expands.

## Decision

Select Arm A for Phase 1. The production implementation is now C23. litehtml remains the deliberate C++ exception at the future rendering boundary, isolated behind a C ABI probe. Arm B and B′ remain useful regression and architecture-comparison targets rather than being discarded.

## C23 port note

The 2026-09-15 port moved the application and extraction pipeline from C++ to C23. The C23 headless runtime uses the linkedom-compatible JavaScript DOM surface and retains Lexbor for native extraction; the historical Arm A measurements above describe the earlier native-binding proof of concept and have not been re-benchmarked. The corpus behavior remains green, while litehtml is still exercised through the isolated C++ adapter in Arm B′.
