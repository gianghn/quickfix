# KRX fork baseline (plan item 1.8)

The reference later changes are measured against (docs/MAINTENANCE.md §2 step 6: p99.9 may not
regress by more than 10 %). Re-measure on the same machine, interleaving old and new builds.

## Latency and throughput, 11 Nov 2026

`krx_bench` (`src/C++/test/krx/Bench.cpp`): ThreadedSocketInitiator → ThreadedSocketAcceptor on
127.0.0.1, FIX.4.4 NewOrderSingle, FIX44.xml validation on, no message log; 200,000 orders for
throughput, 20,000 echoed round trips (after 2,000 warm-up) for latency. Release builds,
Apple Silicon Mac, 16 hardware threads, laptop not isolated: the tails (p99.9, max) are noisy.
Median of three interleaved runs each:

| Store | Engine | msg/s end-to-end | RTT p50 µs | p99 µs | p99.9 µs |
|---|---|---|---|---|---|
| memory | `25273cc3` (before the fork's V1 work) | 266,637 | 26.5 | 46.3 | 72.8 |
| memory | `krx/main` @ `df6beac8` (incl. P2-5 field copies, K-3, K-4, K-6) | 274,098 | 25.7 | 45.0 | 67.7 |
| file | `25273cc3` | 63,449 | 61.2 | 115.5 | 206.1 |
| file | `krx/main` @ `df6beac8` | 62,425 | 60.0 | 87.3 | 159.5 |

No regression beyond the noise (file throughput −1.6 %). The file store has no `fsync` (P0-5);
Phase 4 re-measures with `FileStoreSync` on and off.

Commands (from the repo root, Release build in `build/rel`):

```sh
cmake -S . -B build/rel -DCMAKE_BUILD_TYPE=Release -DQUICKFIX_EXAMPLES=OFF
cmake --build build/rel --target krx_bench
build/rel/src/C++/test/krx/krx_bench mem  200000 20000 7821 /tmp/bench
build/rel/src/C++/test/krx/krx_bench file 200000 20000 7822 /tmp/bench
```

For an older engine, compile `Bench.cpp` against that build's `libquickfix` with
`-DKRX_SPEC_DIR=\"<repo>/spec\"` and its `src/C++` include path.

## FileStoreSync, 12 Nov 2026 (plan item 4.4, U-9)

`krx_bench file` against `krx_bench sync` (the same with `FileStoreSync=Y`), Release, 20,000
orders and 5,000 round trips per run (sync is slow), the same Mac, three interleaved runs, medians.
A round trip persists two messages (one each side), each three file syncs. macOS `fsync` does not
flush the drive's cache, so these are not the production numbers: plan item 11.7 measures them
on the production disk with Linux `fdatasync`.

| Store | msg/s end-to-end | RTT p50 µs | p99 µs | p99.9 µs |
|---|---|---|---|---|
| file, sync off | 66,935 | 59.6 | 88.4 | 118.0 |
| file, `FileStoreSync=Y` (#40: 3 syncs per sent, 1 per received message) | 12,677 | 200.9 | 252.4 | 372.4 |

Option B (#41, owner, 12 Nov 2026): only the body is synced, one sync per sent message and none
per received one; the header and the next sender seqnum are rebuilt from the body after a crash.
Same method, a later set of interleaved runs (one sync run had a 13 ms disk stall in its tail):

| Store | msg/s end-to-end | RTT p50 µs | p99 µs | p99.9 µs |
|---|---|---|---|---|
| file, sync off | 65,775 | 59.8 | 97.2 | 147.9 |
| file, `FileStoreSync=Y`, option B | 27,124 | 102.8 | 127.5 | 195.1 |

## Red list (rows that failed before their fix)

Each fix-phase row was red on `25273cc3`; the red output is in its PR: rows 1.1–1.2 (#4), 1.4
(K-3, #6), 1.5 (P2-5, #7), 1.6 (K-4, #8), 1.3 (#5, #9), 1.7 (K-5, #10), 1.8 (K-6, #12). Rows of
Phases 2–8 record their red runs in their own PRs.

## Sanitizers, 11 Nov 2026

Linux GCC ASan+UBSan+LeakSanitizer, `ut` and `krx_ut`: 0 reports, 0 leaks (on `25273cc3`: 64
UBSan reports, ~2.7 MB leaked, 1 failure). TSan: `ut`, `krx_ut`, FIX.4.4 acceptance clean.

## Mutation scores

Mull 0.35.0 (LLVM 18), the critical files in `mull.yml`, mutants killed by `krx_ut` and `ut`
together (`test/krx/mutation-tests.sh`), 11 Nov 2026, Linux aarch64 in Docker:

| Mutants read from | Mutants | Killed | Score |
|---|---|---|---|
| `krx_ut` (the engine code it links: session, socket threads, parser, message) | 158 | 94 | 59 % |

`krx_ut` alone killed 24 %. The nightly CI run (`mutation` job) adds the mutants read from `ut`
and reports both; per-function scores and the surviving mutants are in its annotations. The PR gate
is 80 % on the lines a PR changes.

Full run on `agent/v1-fix-mull-store` (#45, 13 Nov 2026), the first with each `ut` run in its own
folder. Earlier full runs overstated the score: once a mutant left a corrupt store file in
`test/store`, every later run of `ut` crashed and Mull counted that as a kill.

| Mutants read from | Mutants | Survived | Score |
|---|---|---|---|
| `krx_ut` | 513 | 253 | 50 % |
| `ut` | 454 | 218 | 51 % |

Owner, 13 Nov 2026 (Q10): report only; the score may not drop between releases.
