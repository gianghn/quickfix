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

## Fewer store system calls, 13 Nov 2026 (plan item 15.1, U-16)

`krx_bench file` and `sync`, 200,000 orders and 20,000 round trips, Release, Mac M4 Max, five
interleaved before/after runs. Another session loaded the machine (load average 7–8), so the medians
are noisy (msg/s ranged 45 k–67 k for the same build); best of 5 shows the change better.

| Store | before: best / median msg/s | after: best / median msg/s | RTT p50 µs (median) before → after | p99.9 µs (median) before → after |
|---|---|---|---|---|
| file, sync off | 66,840 / 63,562 | 71,833 / 61,986 | 61.9 → 59.1 | 196.6 → 134.3 |
| file, `FileStoreSync=Y` | 25,662 / 15,362 | 27,730 / 23,003 | 140.9 → 104.5 | 3,867 → 4,296 (disk stalls both sides) |

To measure again on a quiet machine before the PR merges (D9: no p50/p99.9 regression over 10 %).

## Linux, lab cluster, 13 Nov 2026 (plan items 15.1, 15.3, 15.4; U-16, U-17)

One pod (namespace `krx-bench`, `ubuntu:24.04`, g++ 13.3, kernel 7.0, request 150m / limit 6 CPU:
the nodes were at 98 % of their CPU requests with Phase 8's chaos jobs, so the pod ran on spare
CPU) on `rke2-node3`; the store on the pod's `emptyDir`: ext4 on the node's LVM, a consumer NVMe
(Kingston NV3, no power-loss protection). `fdatasync` there takes about 1 ms, so `FileStoreSync=Y`
is about 1 k msg/s, not the Mac's 27 k (macOS `fsync` does not flush the drive cache).

15.1, interleaved before (`agent/v1-p8e`) and after: file 5 runs of 100,000 orders and 5,000 round
trips; sync 3 runs of 3,000 orders and 300 round trips. Medians.

| Store | msg/s before → after | RTT p50 µs | p99 µs | p99.9 µs |
|---|---|---|---|---|
| file, sync off | 107,449 → 194,177 | 37.5 → 21.1 | 275.5 → 446.7 | 668.2 → 1,133.9 |
| file, `FileStoreSync=Y` | 1,080 → 1,255 | 1,428 → 1,456 | 8,986 → 8,115 | 13,142 → 9,141 |

The file store's p99/p99.9 rose on a node shared with chaos jobs; to measure again on a quiet node
before 15.1 merges (D9).

Group commit spikes (`spike/v1-p15-group-commit`, not for merge), `FileStoreSync=Y`, 5,000 orders
and 300 round trips, 3 interleaved runs, medians; "sync" is today's one sync per message (with 15.1).
Messages per sync is over the whole run (warm-up and round trips sync one by one); in the
throughput phase 1a put about 125 messages in a sync, 1b about 14 at 16 senders.

| Senders | sync msg/s | 1a msg/s | 1b msg/s | RTT p50 µs sync / 1a / 1b |
|---|---|---|---|---|
| 1 | 1,323 | 172,985 | 1,216 | 1,565 / 1,993 / 1,757 |
| 4 | 1,033 | 93,211 | 2,773 | 1,719 / 1,679 / 1,568 |
| 8 | 1,176 | 113,939 | 5,182 | 1,397 / 1,506 / 1,588 |
| 16 | 1,381 | 110,371 | 10,810 | 1,379 / 1,432 / 1,594 |

The Mac with a sync delay added (`KRX_SYNC_DELAY_US`, a stand-in for a slower disk) agrees: at
1 ms, sync 704 msg/s, 1a 95,331, 1b 697 / 1,545 / 2,862 at 1 / 4 / 8 senders; at 200 µs, sync
3,164, 1a 88,273, 1b 3,159 / 6,855 / 13,285.

## Rocky Linux 9, lab cluster, 13 Nov 2026 (plan items 15.1, 15.3, 15.4; U-16, U-17)

The production OS's userspace: a `rockylinux:9` (9.3) pod, GCC 13.3 (gcc-toolset-13, as CI's
Rocky job), on `rke2-node1`, a quieter node; the store on that node's NVMe (ext4). A container
shares its node's kernel, so this is Ubuntu's kernel 7.0 and ext4, not Rocky 9's 5.14 and XFS:
11.7 still measures the production host. Same runs as the Linux section above, medians.

| Store | msg/s before → after 15.1 | RTT p50 µs | p99 µs | p99.9 µs |
|---|---|---|---|---|
| file, sync off | 137,718 → 253,809 | 38.7 → 30.5 | 76.0 → 56.0 | 377.9 → 346.7 |
| file, `FileStoreSync=Y` | 1,474 → 1,504 | 1,394 → 1,332 | 2,145 → 1,724 | 2,708 → 1,908 |

On the quiet node 15.1 makes no percentile worse (D9 holds; the Ubuntu run's p99.9 was the busy
node).

| Senders | sync msg/s | 1a msg/s | 1b msg/s | RTT p50 µs sync / 1a / 1b |
|---|---|---|---|---|
| 1 | 1,554 | 244,526 | 1,422 | 1,297 / 1,394 / 1,395 |
| 4 | 1,555 | 183,378 | 3,131 | 1,325 / 1,299 / 1,420 |
| 8 | 1,521 | 87,470 | 6,106 | 1,298 / 1,437 / 1,391 |
| 16 | 1,523 | 105,412 | 11,302 | 1,280 / 1,387 / 1,460 |

Where a message's time goes (the spike's `KRX_STEP_STATS` timers; `strace` is blocked by the
node's AppArmor), mean µs per message:

| Step | file, 1 sender | sync, 1 sender | 1a, 1 sender | 1a, 8 senders | 1b, 8 senders |
|---|---|---|---|---|---|
| session lock wait | 0.02 | 0.05 | 0.06 | 39.3 | 16.6 |
| build (toApp, toString) | 0.44 | 1.5 | 0.78 | 0.99 | 2.4 |
| body write | 0.36 | 3.6 | 1.6 | 1.9 | 7.8 |
| body sync (in `send()`) | — | 612.9 | — | — | — |
| header write | 0.27 | 3.2 | 0.51 | 0.71 | 1.7 |
| seqnums write (sent) | 0.31 | 1.8 | 0.67 | 0.78 | 1.8 |
| socket write | 1.23 | 7.8 | (flusher) | (flusher) | (flusher) |
| **`send()` total** | **3.4** | **636.3** | **6.8** | **47.9** | **959.6** |
| flusher: sync per batch | — | — | 627.1 | 635.0 | 615.9 |

Sync mode is 96 % `fdatasync`; the engine's own work is about 20 µs. 1a's senders never wait for
the disk; with 8 of them, 82 % of `send()` is waiting for the session lock, which is why 1a
is slower at 8–16 senders than at 1. 1b's senders wait about 1.5 syncs (the one in progress, then
theirs). `perf`: sync mode uses 0.25 CPU over 8 s (the process waits for the disk), file mode
0.57; the engine's top functions are dictionary validation (2–5 %) and time formatting (about
4 %).

Batch send (the spike's `Session::sendToTargetBatch`: the batch stored under one hold of the
session lock without a sync, one `fdatasync`, then the socket writes in order; no extra thread),
one sender, `FileStoreSync=Y`, the same Rocky pod, 3 interleaved rounds, medians. "Call" is one
`sendToTargetBatch`: every order in the batch is on disk and written when it returns.

| Batch | msg/s | × today | call p50 µs | call p99 µs | syncs per order |
|---|---|---|---|---|---|
| 1 (today's `send()`) | 1,544 | 1.0 | 647 (one send) | — | 1 |
| 4 | 5,542 | 3.6 | 680 | 1,058 | 1/4 |
| 16 | 18,271 | 11.8 | 782 | 1,333 | 1/16 |
| 64 | 56,800 | 36.8 | 980 | 1,437 | 1/64 |

Step timers at 64: the sync per batch 721 µs, the socket writes 143 µs (2.2 µs an order), the store
writes about 3 µs an order. Throughput is batch ÷ (sync + about 5 µs × batch): 90 % of the ideal
at 4, 57 % at 64.

Background flush (15.6 spike, 14 Nov 2026: store sync off, a thread `fdatasync`s body and seqnums
every `KRX_FLUSH_MS`; the loss window is the age of the oldest unflushed write when its flush
ends), file mode, 100,000 orders and 5,000 round trips, the same Rocky pod, 3 interleaved runs,
medians:

| Flush every | msg/s | `send()` µs | RTT p50 µs | loss window p50 / p99 / max ms | flush p50 ms |
|---|---|---|---|---|---|
| never (file mode) | 230,887 | 3.45 | 27.0 | up to the kernel's ~30 s | — |
| 1 ms | 216,243 | — | 27.6 | 3.3 / 4.8 / 5.3 | 1.1 |
| 5 ms | 245,157 | 3.54 | 32.3 | 7.7 / 9.2 / 9.4 | 1.3 |
| 10 ms | 225,285 | — | 31.6 | 13.3 / 14.8 / 18.5 | 1.6 |

The send path does not notice the flusher (within the run-to-run noise). A flush is two
`fdatasync`s (body, seqnums), so the window is about the interval plus up to two flushes; the
spike sleeps a full interval after each flush, which the build should not (sleep to the next tick).

15.6 as built (14 Nov 2026), the same Rocky pod: before = 15.1 (90665b83, no flusher), after =
26c79293, file mode, 100,000 orders and 5,000 round trips, 3 interleaved rounds (the before build ran
twice a round), medians:

| Build | msg/s | RTT p50 µs | p99.9 µs |
|---|---|---|---|
| before (6 runs) | 259,776 | 26.4 | 346 |
| after, `FileStoreFlushInterval=0` | 277,588 | 30.2 | 308 |
| after, `FileStoreFlushInterval=5` | 244,442 | 31.3 | 236 |

Throughput and p99.9 hold. The p50 median is 15 % higher, over D9's 10 % on paper, but single runs
of one build range 22–32 µs and with the flush off the send path only gains an atomic check; to
measure again with more runs before 15.6 merges.

## 15.6 and 15.7 as built, rke2-node3, 14 Nov 2026

The Rocky 9 pod moved to `rke2-node3` (node1 was at 99 % CPU with Phase 8's TSan soak jobs; node3
at 61 %, still running three soak jobs, so expect noise). Store on the node's LVM ext4 (consumer
NVMe). Before = 15.1 (90665b83), after = 0780e5f9. Interleaved rounds, medians; 300 round trips a
run, so p99.9 is rough.

15.7, `FileStoreSync=Y`, one sender, 3 rounds:

| Batch | msg/s | × today | call p50 ms | call p99 ms |
|---|---|---|---|---|
| 1 (today's `send()`) | 1,110 | 1.0 | — | — |
| 4 | 3,730 | 3.4 | 0.89 | 2.7 |
| 16 | 11,768 | 10.6 | 1.14 | 3.0 |
| 64 | 32,243 | 29.0 | 1.43 | 5.2 |

15.6, file mode, 5 rounds:

| Build | msg/s | RTT p50 µs |
|---|---|---|
| before (15.1) | 179,485 | 22.7 |
| after, `FileStoreFlushInterval=0` | 189,520 | 21.3 |
| after, `FileStoreFlushInterval=5` | 134,061 | 23.3 |

With the flush off, 15.6 does not regress (D9 holds; the earlier +15 % p50 was noise). With the
flush on, throughput is 25 % lower on this busy node (6 % on the quiet one) and 2 of 5 runs had a
p99 near 3 ms: on ext4 an `fdatasync` of a growing file commits the journal, and an append that
grows the file can wait for that commit. Pre-sizing the body file (`fallocate`) would keep most
appends from changing the file's size; XFS (Rocky 9's default) may behave differently. To check
on the production disk (11.7).

Pre-size spike (14 Nov 2026, `spike/v1-p15-presize` 0aab7f55, not for merge): `open()` pre-sizes the
body and header files by 256 MB (the runs fill about 17 MB) with `posix_fallocate` or by writing
zeros, then syncs. File mode, 100,000 orders and 5,000 round trips, 5 interleaved rounds, medians;
ext4 on LVM (consumer NVMe).

| Node (CPU busy) | Flush | Pre-size | msg/s | RTT p50 µs | p99 µs | p99.9 µs |
|---|---|---|---|---|---|---|
| node2 (18 %) | off | none | 242,636 | 30.7 | 84.7 | 445 |
| node2 | off | fallocate | 248,320 | 28.6 | 132.4 | 697 |
| node2 | off | zeros | 157,298 | 34.7 | 131.4 | 617 |
| node2 | 5 ms | none | 251,515 | 29.0 | 90.4 | 525 |
| node2 | 5 ms | fallocate | 223,667 | 30.5 | 121.7 | 624 |
| node2 | 5 ms | zeros | 150,611 | 29.7 | 202.0 | 632 |
| node3 (35 %) | off | none | 165,587 | 25.8 | 71.4 | 609 |
| node3 | off | fallocate | 166,318 | 25.9 | 101.0 | 625 |
| node3 | off | zeros | 118,113 | 33.8 | 240.5 | 626 |
| node3 | 5 ms | none | 158,171 | 27.1 | 149.8 | 690 |
| node3 | 5 ms | fallocate | 161,743 | 26.6 | 96.8 | 640 |
| node3 | 5 ms | zeros | 116,937 | 34.1 | 339.0 | 655 |

The 5 ms flush cost 0 % (node2) and 4.5 % (node3) here, with no 3 ms p99 in 30 runs; the −25 %
above came with three soak jobs on node3. `fallocate` is within the noise either way; writing over
zero-filled blocks is 30–37 % slower than appending, flush or not. Not built.

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

## Chaos and long runs (Phase 8), lab Kubernetes cluster

`krx_stress` (`src/C++/test/krx/Stress.cpp`) runs on the lab cluster (owner decision Q12): RKE2,
3 × 16 CPU, namespace `krx-soak` with its own quota. `test/krx/k8s/run.sh start NAME VARIANT RUNS
PARALLEL -- ARGS` starts one Indexed Job per suite and one pod per seed (seed = pod index + 1);
each pod clones the commit and builds its variant (Release, ASan+UBSan, TSan). `run.sh results
NAME` prints one line per seed. "N of M finished" counts only the runs that ended before
the batch was replaced. A chaos run: 90 s, 4 sessions, 3 sender threads each, chaos steps
every 150–500 ms; `--no-echo` (orders only) or echo on (the acceptor echoes every order: both sides
resend at once after an outage). PASS means both sides in step after the settle, LOST = DUP = OOO =
UNEXPECTED = 0, no canary file descriptor closed, and every stop within its limit (15 s; 60 s
under a sanitizer).

| Round (commit) | Release | Release, echo on | ASan+UBSan | TSan | Found → fixed |
|---|---|---|---|---|---|
| 1 (`ed96f5e5`, `f824dbca`) | 13 of 15 finished PASS | 14 of 18 finished PASS | 20 of 20 | 3 of 15 | TSan: two reading threads on one acceptor session, across acceptor stops (#59) |
| 2 (`a7532eaa`) | 19 of 20 | 16 of 20 | 20 of 20 | 4 of 15 | TSan: `disconnect()` from another thread raced the reset flags and resend range (#59); OOM: the resend yield and the unbounded queue (Q15, Q16, #60) |
| 3 (`8f1102f7`) | 20 of 20 | 3 of 20 | 18 of 20 finished PASS | 12 of 15 | 1–3 single orders lost per run with both sides in step: a Logout or Reject above a gap (#60) |
| 4 (`6eae9940`) | 17 of 20 (3 without a result) | 18 of 20 | 20 of 20 | 15 of 15 | 2 initiator stops over 15 s: the backlog read while sending was processed after the shutdown (#61) |
| 5 (`26f4e4c2`) | 20 of 20 | 13 of 20 | 15 of 20 (5 without a result) | 15 of 15 | echo on: 5 runs not back in step within 300 s (seeds 5, 11, 18, 19, 20; 0.2–0.8 M orders behind), 1 stop over 15 s (seed 17), on nodes short of CPU; seed 5 passed locally. Open |
| final (`784e1b0c`) | running | running | running | running | 8.3's final batch; 8.4's TSan 1 h × 10 runs beside it |

Also on the research's engine (`25273cc3`, today's harness, ASan): 0 of 10, each stopped first on a
memory error fixed in Phases 2–6, so the research's gap itself was not seen again.

Local numbers (Release, Apple Silicon Mac): a resend of 2000 messages while three senders flood
the session took 20,083 ms before Q15 and 9 ms after. Echo-on seed 16 after Q15 and Q16: PASS,
2.14 M orders each way, peak RSS 1.1 GB (mostly the harness's own per-message records).
