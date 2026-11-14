# V1 plan — KRX fork go-live (`krx-1.0.0`)

A build-only release. It takes the fork from `krx/main` @ `25273cc3` to a build we can certify with
HOSE/KRX and run as DNSE's exchange trade gateway engine (through quickfix-rs, in
`enCapital/krx_gateway_rust`).

Source: the go-live research of 10 Nov 2026,
[report](https://github.com/gianghn/ClaudeResearch/blob/main/quickfix-golive/report/quickfix-golive.md)
(P0/P1/P2 tables and the 14-group checklist),
[harness](https://github.com/gianghn/ClaudeResearch/tree/main/quickfix-golive/harness), and the
[learning page](https://gianghn.github.io/ClaudeResearch/quickfix-golive/). Defect IDs (`P0-1`…)
are the report's. Rows are in [V1_BDD_CHECKLIST.md](V1_BDD_CHECKLIST.md); how we test and maintain
the fork is in [MAINTENANCE.md](MAINTENANCE.md).

## What changed

**Problem.** The happy path is solid: unit (53 cases, 1,954 assertions) and acceptance (468, 467
threaded) suites pass under ASan+UBSan and TSan, and a 90 s Release chaos run moved 2.26 M orders
with nothing lost, duplicated or reordered. FileStore runs at about 65 k msg/s, RTT p50 61 µs,
p99.9 405 µs on a 2-core VM. Throughput is not the risk. The failure paths are.

Causes ranked by impact on a trading day:

1. **Process dies on one input** — forged data-length over-read (P0-1, segfault), pre-logon
   SequenceReset → `std::terminate` (P0-2), malformed frame → stale re-dispatch and logout (P0-7).
2. **Session freezes** — blocking `send()` with no timeout under `Session::m_mutex` (P0-3);
   EINTR on read drops sessions (P1-3); accept loop exits silently (P1-4); heartbeat starved by a
   trickling peer (P1-10).
3. **Silent loss or duplicates** — `send()` returns `true` for unsent orders (P0-6); dictionary
   rejects consume exec-report seqnums (P0-8); quickfix-rs maps a callback panic to success (P0-9);
   an unexplained ~3,700-message gap in 1 of 3 ASan chaos runs (P0-10).
4. **Durability** — one write error breaks FileStore for good, even after restart (P0-4); no
   `fsync` anywhere (P0-5).
5. **Races** — `m_pResponder` use-after-free window at teardown (P0-11); `SessionState` flags,
   `lastSentTime`, hand-written recursive `FIX::Mutex` (P1-1, 130 of 152 TSan reports).
6. **Hygiene and upgrade traps** — P1-5…P1-13, P2-1…P2-6, Trap-1…Trap-3.

Upstream has merged nothing since 21 May 2026 and ~45 PRs (including the security fix #779) sit
unreviewed, so we own the fork. Estimate: about two sprints of fixes plus soak and certification.

## Decisions

### Defaults this plan takes

The owner may change any of these before its phase starts.

| # | Default |
|---|---|
| D1 | Base is `krx/main` @ `25273cc3`. No rebase onto upstream master during V1 (Trap-1, Trap-2). Upstream fixes come in as single cherry-picks, each listed in `KRX_FORK_NOTES.md`. |
| D2 | Library behaviour is specified with Catch2 v3 `SCENARIO`/`GIVEN`/`WHEN`/`THEN` (already vendored as `src/C++/test/catch_amalgamated.*`) in `src/C++/test/krx/`; no new test dependency. Gateway behaviour stays in Gherkin in `krx_gateway_rust/tests/features/`. |
| D3 | The research harness (stress, fuzz, repro A–D, summarizers, crash input) is imported into `test/krx/` in Phase 1 and becomes the fork's own. |
| D4 | Mutation testing uses Mull (LLVM, clang build) on the critical functions listed in `MAINTENANCE.md`; quickfix-rs uses `cargo-mutants`. Gate: mutation score ≥ 80 % on each changed critical function; every survivor is killed by a new test or recorded as equivalent. |
| D5 | Every fix is TDD: a test that fails on `25273cc3` (red count recorded), then the fix, then green in Release, ASan+UBSan and TSan. |
| D6 | One phase, one branch, one PR, stacked: `agent/v1-p<k>` on top of the previous phase; merged in order into `krx/main`. |
| D7 | A FileStore write failure fails closed: the session stops taking orders, disconnects and logs an error the gateway alerts on. |
| D8 | Fixes that change public C++ behaviour keep the method signatures (quickfix-rs binds them); new behaviour is opt-in by setting only where the old behaviour is still safe. |
| D9 | Thresholds marked *proposed* in the report (≤ 10 % p99.9 regression, 2× peak soak, 5 s alerts) are used until HOSE's member documents give the real numbers. |
| D10 | The repo stays a public fork, so CI runs on GitHub-hosted runners (`ubuntu-latest`, `macos-latest`; sanitizer and Mull jobs on `ubuntu-latest` with clang). Runs longer than a job allows (24 CPU-h fuzzing, 8 h soak, 1 h TSan × 10 seeds) run in the lab and attach their logs to the phase handover. |
| D11 | Send timeout (P0-3, owner 12 Nov 2026): one message may take `SendTimeout` seconds to send, default **half of HeartBtInt** (15 s at KRX's usual 30; 15 s while HeartBtInt is still unknown). Past it the send fails, the socket is shut down and the session disconnects. |

### Only the owner can decide

| # | Question | Options | Recommended | Blocks | Answer |
|---|---|---|---|---|---|
| Q1 | How do we make the store durable against host crash or power loss (P0-5)? | opt-in `fdatasync` group commit plus reconciliation runbook · reconciliation runbook only · `fdatasync` only | opt-in `fdatasync` group commit plus reconciliation runbook | Phase 4 | **sync plus runbook** (owner, 12 Nov 2026); if the sync costs too much speed, we look at it again |
| Q2 | May `Session::send()`/`sendToTarget` return `false` when the message was stored but not written to the socket (P0-6)? quickfix-rs and the gateway are the only callers we know. | return `false` when not written · keep `true`, add a new "sent" query · keep as is, gateway checks `is_logged_on()` only | return `false` when not written | Phase 5 | **return `false`** (owner, 12 Nov 2026); the gateway refuses PossDup orders in `toApp` so a stored order never goes out late (Phase 10) |
| Q3 | What happens to an inbound application message that fails dictionary validation (P0-8)? | dictionary superset of the HOSE spec plus alert on every 35=3/35=j · also patch the engine to deliver it to `fromApp` with a flag · validation off for app messages | dictionary superset of the HOSE spec plus alert on every 35=3/35=j | Phase 5 | **full dictionary plus alerts** (owner, 12 Nov 2026); the KRX spec is `quickfix-rs/quickfix-msgkrx/src/FIX44KRX.xml` (32 messages, 978 fields) |
| Q4 | What does the gateway do when a callback panics (P0-9)? | abort the process (`panic = "abort"`) · disconnect the session and keep the process | abort the process (`panic = "abort"`) | Phase 9 | **alert the monitoring, then abort** (owner, 12 Nov 2026): the panic is logged and raised as an alert before the process stops |
| Q5 | Does the gateway run with `NonStopSession=Y`? If so #783 becomes P0 (P1-8). | no · yes | no | Phase 7 | **no** (owner, 12 Nov 2026): operations restart the gateway every day; #783 stays P1 |
| Q6 | How is the exchange link encrypted (P2-2)? | no TLS on the leased line · stunnel/HAProxy in front · built-in ThreadedSSL with #677/#751/#713 | stunnel/HAProxy in front | Phases 7, 13 | **no TLS** on the leased line (owner, 12 Nov 2026): 7.4 (the SSL picks) is not needed; Phase 13 records the unencrypted link as an accepted risk |
| Q7 | Do we offer our fixes upstream (public PRs)? | yes, after go-live · yes, as each lands · no | yes, after go-live | Phase 12 | **no** (owner, 12 Nov 2026): fixes stay in the fork |
| Q8 | Who signs the P1 risk records and the go/no-go (engineering, risk, compliance, operations)? | named approvers per area · CTO alone | named approvers per area | Phase 14 | **the CTO alone** (owner, 12 Nov 2026) |
| Q9 | The nightly mutation job takes over 2 of its 3 hours: how do we keep it inside the limit? | one job per test binary · a 6 h limit · fewer mutants nightly, all weekly | one job per test binary | CI | **one job per binary** (owner, 13 Nov 2026) |
| Q10 | What mutation score must the whole critical code reach (50–51 % now)? | report only, never drop · 70 % by release · 80 % by release | report only | Phase 12 | **report only** (owner, 13 Nov 2026): the PR gate stays 80 % on changed lines; the nightly score is recorded in `test/krx/BASELINE.md` and may not drop between releases |
| Q11 | When are the initiator's `stop()` order and `onStop` spin fixed (the acceptor's were, 6.5 and 6.6)? | Phase 8.7, red first · now without a red test · not at all | Phase 8.7, red first | Phase 8 | **Phase 8.7, red first** (owner, 13 Nov 2026) |
| Q12 | Where do Phase 8's long runs (chaos, TSan hours, soak, fuzzing) run? | a lab machine · GitHub Actions · this Mac in Docker | a lab machine | Phase 8 | **the lab Kubernetes cluster** (owner, 13 Nov 2026): RKE2, 3 × 16 CPU; namespace `krx-soak` with its own quota; a privileged init container only sets `vm.mmap_rnd_bits=28` for TSan; Jobs started by a script in `test/krx/k8s/` |
| Q13 | Keep the decisions taken without the owner in Phases 6–7? | keep · revisit | keep | — | **keep** (owner, 13 Nov 2026): the `GapFill to N dropped K queued messages` event (7.1), the #755 pick (7.6), `onLogout` after the session reset and outside the lock (6.4) |
| Q14 | Echo-on chaos found a livelock: after an outage both sides resend from their reading threads, neither reads, both sends time out, again on every reconnect. Fix it? | fix in 8.5, red first · document as a known limit · defer to a later release | fix in 8.5, red first | Phase 8 | **fix in 8.5, red first** (owner, 13 Nov 2026): the reading thread keeps reading while it waits to send (U-6) |
| Q15 | Under a steady order flow a large resend crawls (about 100 messages/s: P0-3 yields up to 10 ms before every message); the peer queues everything sent meanwhile (1.9 GB, OOM at 6 GiB, stops over 15 s). How do resend and senders share the session (item 3.8)? | bounded yield · senders wait for the resend · keep the yield, throttle the harness | bounded yield | Phase 8 | **bounded yield, red first** (owner, 13 Nov 2026): senders get in once per millisecond of resending, for up to a millisecond; row 3.5 still holds. Closes 3.8's yield decision |
| Q16 | Messages above a gap are queued without a limit. Cap the queue? | cap and drop · no cap, document · defer to Phase 11 | cap and drop | Phase 8 | **cap and drop, red first** (owner, 13 Nov 2026): at most 10,000 per session; the rest are dropped with an error event and come back with the resend (row U-9). The cap is the session setting `MaxQueuedMessages` (owner asked, 13 Nov 2026; 0 for no limit) |
| Q17 | A resend loads its whole range from the store before it sends; a large one (340 k messages) outlasts a counterparty's send timeout when both resend, so 4 of 20 echo-on runs never caught up. How should the resend load? | in chunks · keep it, slower heartbeat in the harness · document only | in chunks | Phase 8 | **in chunks, red first** (owner, 14 Nov 2026): 1000 messages at a time |
| Q18 | With `FileStoreSync=Y` each message waits for its own disk sync, in turn, under the session lock: about 1.5 k msg/s on Linux (Rocky 9 userspace, a lab NVMe; the Mac's 25 k is a cache-only `fsync`) (Phase 15). How do messages share a sync? | 15.3 decoupled (1a): `send()` returns once written, a flusher syncs a batch and only then puts it on the wire · 15.4 shared (1b): senders on one session wait for one sync together, `send()` keeps its meaning · batch send: the gateway hands a batch to one call, one sync, `send()` keeps its meaning, no extra thread · neither: one sync per message | batch send if the gateway can drain its order queue into one call (37× at 64, no new thread, `send()` unchanged); 1a only if more is needed (about 100× from one sender, but `send()` changes meaning and needs a "not sent" event); either way only if 11.7's production disk syncs in more than about 100 µs | Phase 15 | **batch send with sync, background flush without** (owner, 14 Nov 2026): `FileStoreSync=Y` uses batch send (15.7), nothing reaches the exchange before it is on disk; `FileStoreSync=N` gets a background `fdatasync` every few ms (15.6), a short loss window on host crash, handled by the Q1 runbook. 1a and 1b are not built. **Production (owner, 14 Nov 2026): background flush**, `FileStoreSync=N` with `FileStoreFlushInterval=5`; batch send stays in the library for sync mode |

## Ground rules

- Phases follow `build-phase`; reviews follow `review-pr`; the BDD checklist follows `bdd-checklist`.
- Every change follows [MAINTENANCE.md](MAINTENANCE.md): red test first, sanitizer matrix, edge-case
  table and mutation run on critical functions, `KRX_FORK_NOTES.md` ledger line.
- Special to V1:
  - Each P0 row's test must fail on `25273cc3`; the handover records the red output.
  - Measure before and after any change on the send, store or socket path (the `bench` from
    Phase 1): p50/p99/p99.9 must not regress by more than 10 % (D9).
  - Phases 9–14 run in other repos or are non-code; they are tracked here so the go/no-go has one
    list. Their items name the repo or the owner.

## Progress

| Phase | What | Items | From | Status |
|---|---|---|---|---|
| 1 | Test harness, sanitizer and mutation CI | 1.0–1.9 | static analysis, fuzzing groups, P2-5, P2-6, K-3 | done (#4–#18); per-function Mull scores (U-2) come from the nightly run |
| 2 | Hostile input never kills the process | 2.1–2.7 | P0-1, P0-2, P0-7, P1-11, P1-13 | done (#19–#23) |
| 3 | Sockets never freeze a session | 3.1–3.7 | P0-3, P1-3, P1-4, P1-10, P1-12 | done (#25–#35); 3.8 (benchmark) later |
| 4 | Store survives disk errors and crashes | 4.1–4.5 | P0-4, P0-5, P2-4 | done (#36–#41); the crash runbook (Q1) to write |
| 5 | No silent loss | 5.1–5.4 | P0-6, P0-8 | done (#42–#44); 5.4 not needed (Q3) |
| 6 | Race-free teardown and locking | 6.1–6.6 | P0-11, P1-1, P1-2, P1-7, P2-3 | done (#46–#50; 6.3 itself #28, #29) |
| 7 | Upstream cherry-picks and hygiene | 7.1–7.7 | P1-5, P1-6, P1-8, P2-1, P2-2 | done (#51–#55); 7.3 and 7.4 not needed (Q5, Q6) |
| 8 | Sequence-gap root cause and long runs | 8.1–8.7 | P0-10, concurrency and fuzzing groups | in progress: 8.1, 8.2, 8.5 fix, 8.7 in #57–#59; the long runs (8.3–8.6) on the lab cluster |
| 9 | quickfix-rs fork fails closed | 9.1–9.5 | P0-9, Trap-3 | not started |
| 10 | Gateway integration and conformance | 10.1–10.8 | P0-6, P0-8, P1-2, conformance group | not started |
| 11 | Fault injection and performance | 11.1–11.8 | fault and performance groups | not started |
| 12 | Fork release and governance | 12.1–12.7 | governance, build hardening | not started |
| 13 | Operations, HA, security and compliance | 13.1–13.8 | ops, HA, security, regulatory groups | not started |
| 14 | Certification and go-live | 14.1–14.6 | certification, go-live groups | not started |
| 15 | Store throughput | 15.0–15.7 | Q1 ("if the sync costs too much speed, we look at it again") | built on `agent/v1-p15a` (not yet merged): 15.1, 15.6, 15.7; 15.3, 15.4 and a pre-size spike measured, not built; 15.2 open (Handover 15) |

### Next steps (13 Nov 2026)

1. ~~**Owner:** merge the Phase 3–5 stacks~~ merged (#31–#44, 12 Nov 2026). #45 fixes `krx/main`'s nightly mutation job (`ut` crashed on store files a mutant left behind).
2. ~~3.7 P1-12~~ done in #34 (row 3.11). The gateway needs a short `ReconnectInterval` (1–2 s) for a 5 s failover.
3. ~~Row 3.2~~ test in #35 (a guard: it already held).
4. 3.8: the resend-under-contention benchmark, then the yield decision with the owner (not urgent: 4–8 cores, no CPU limit).
5. ~~Phase 3 handover~~ written (Handover 3, in #35).
6. ~~Phase 6~~ merged (#45–#50, Handover 6). ~~Phase 7~~ merged (#51–#55, Handover 7). Next: Phase 8 (gap root cause, long runs on the lab Kubernetes cluster, Q12; 8.7 also takes the initiator's stop order and `onStop` spin, Q11). The nightly mutation job runs one job per binary (Q9).
7. Q1–Q8 answered (12 Nov 2026), so no owner question blocks a phase now: Phase 4 (sync plus runbook) and Phase 5 can start after Phase 3. The gateway has 4–8 cores and no CPU limit, so 3.8 is not urgent. The KRX dictionary is `quickfix-rs/quickfix-msgkrx/src/FIX44KRX.xml`; HOSE throttles and the certification script are still to be located (Phases 10, 11, 14).

## Phases

Independence: Phase 1 first. Phases 2, 3, 4 and 7 are independent of each other once Phase 1 is
in (stack them in this order to keep the merges simple). Phase 5 needs 3 (send path). Phase 6
needs 3. Phase 8 needs 2–7. Phase 15 needs 4 and stacks on Phase 8 (its harness); it ends before
Phase 11 measures. Phase 9 can start any time. Phase 10 needs 5 and 9. Phase 11 needs 8
and 10. Phase 12 needs 8. Phases 13 and 14 run alongside 11–12 and finish last.

### Phase 1 — Test harness, sanitizer and mutation CI · **medium** · `quickfix`

Nothing else can be TDD'd until this exists.

- 1.0 Put the fork under the house C++ policy in fork mode (Local_Claude_Setup C-D15): `.cpp-policy-fork` with `base=` upstream `94625580`, `set(HOUSE_FORK ON)` and `include(HouseFlags.cmake)` in the top `CMakeLists.txt`, upstream library targets marked `SYSTEM`, every new target through `house_target()`; `lint.sh .` passes.
- 1.1 Import from the research harness what Phase 1 needs, rewritten to the house rules in `src/C++/test/krx/`: `krx_bench` (from `bench.cpp`) and the libFuzzer entry `krx_fuzz` (`-DKRX_FUZZ=ON`, clang; the modes are `FixInput`, from `fuzz_message.cpp`). The rest goes where it is used: `stress.cpp`/`run_stress.sh` to Phase 8 (8.1 reworks them for P0-10); repro A–D and `repro_datalen_overread.cpp` become the red scenarios of Phases 2–5; `make_corpus.py` is `SeedsFromDefinitions`; the summarizers are not needed (CI fails on the first report); `ubsan_vptr.supp` is gone (P2-5 fixed the cause).
- 1.2 Our own scenario target `krx_ut` (`src/C++/test/krx/`, Catch2 built as third-party code, `house_target`), with the `SessionPair` fixture: an acceptor `Session` behind a real `ThreadedSocketConnection` on a socketpair, driven by bytes, reused by Phases 2–6.
- 1.3 `FixInput`: the fuzz modes as a library, and fuzz seeds made from the acceptance `.def` files at test time; `krx_ut` replays every seed (no libFuzzer needed). The fuzzer's crash input for P0-1 lands with its fix in Phase 2 (row 2.4): replaying it today crashes the parser.
- 1.4 Fix the test bugs that stop sanitizer runs: `UtilityTestCase.cpp:224` detach-after-join, the thread `spawnThread_True` never joins, leaks at `FileStoreTestCase.cpp:200` and `UtilityTestCase.cpp:72` (P2-6).
- 1.4a K-3 (found reading LSan's stacks in Phase 1): `DataDictionary::operator=` keeps the groups it held that the right side doesn't have (a dictionary assigned over another validates with stale groups), leaks the ones it replaces, and has no self-assignment check (`DataDictionary.cpp:80-121`). Test-first in `krx_ut`; the groups are owned by `unique_ptr`, cleared before the copy (as upstream #764 does for `FieldMap`).
- 1.4d K-4 (found by UBSan after 1.4b; owner, 11 Nov 2026: library fix): the generated message classes (`src/C++/fix4*/Message.h`, `fix50*/`, `fixt11/`) returned their header and trailer as `(Header&)m_header`, casting a `FIX::Header` to the version's `Header` type: undefined behaviour for every caller of a typed message's `getHeader()`/`getTrailer()` (17 UBSan reports left in `ut`). `FIX::Header`/`FIX::Trailer` get the typed `set`/`get`/`isSet`/`getIfSet` as templates; the generator (`spec/GeneratorCPP.rb`) and the 9 generated files drop the casting overrides, so a typed message returns the real `FIX::Header&`. The version `Header` classes stay for their nested group types. Breaks only code that binds a `FIX44::Header&` (none in this repo, none in quickfix-rs, which uses the generic `Message`).
- 1.4e K-5 (found under Linux ASan in Phase 1; also on `krx/main` @ `25273cc3`): `SessionTestCase` `processQueuedMessages` got one ResendRequest instead of two. Root cause: a test artifact, not the engine. The test's messages carry the real time (`fillHeader`) while the session runs on the fixture's fixed clock; under ASan the section takes 145 s, past `MaxLatency` (120 s), so the last message was rejected as a SendingTime accuracy problem instead of starting a resend. The section turns the latency check off (it tests the queue). Not related to P0-10.
- 1.4f K-6 (found by CI's LeakSanitizer job in 1.5): the plain `SocketAcceptor` driven by `poll()` never freed its `SocketServer` (only the threaded `onStart` path did), so a destroyed acceptor kept its listening port; and `SocketServer::close()` closed the listening sockets without forgetting them, so `onStop`, `onStart`'s end and `~SocketMonitor` could close the same number up to three times (the fork notes' known plain-acceptor issue; Trap-3). The server is a `unique_ptr`; `close()` drops each listening socket through the monitor (one close) and forgets it.
- 1.4c The unit suite's LeakSanitizer report on Linux (~2.7 MB): 128 direct leaks were UBSan's own report printer (gone with 1.4b, 1.4d); the rest came from seven `SessionTestCase` sections that build their own session with `object = new Session(...)` over the one the fixture's constructor made, leaking it while it still held the session ID in the registry. The fixtures get `destroySession()` (a `unique_ptr`, the house rules allow no naked `delete`), called before each overwrite. Result: 0 leaks, 0 UBSan reports in `ut`.
- 1.4b P2-5 (owner, 11 Nov 2026: port now, the house policy allows no suppression file): replace the `FIELD_GET_REF`/`FIELD_GET_PTR`/`getField<T>()` downcasts with the safe `getField(T&)`, as upstream #735 does (hand port; `FieldMap.h`, `Session.cpp`, `Message.cpp`, `MessageCracker.h`, `DataDictionary.cpp`, `Acceptor.cpp`). UBSan's vptr check is then clean in `ut` and `krx_ut`.
- 1.5 `krx_ci.yml`: add ASan+UBSan (with LeakSanitizer) and TSan jobs (Linux) running `ut`, `krx_ut`, acceptance, and threaded acceptance under ASan (under TSan it waits for Phase 6, row 6.3); no suppressions.
- 1.6 Static analysis is the house C++ policy in fork mode (1.0): the fork's own code gets every gate and `-Werror`, upstream lines we change get ast-grep, the backstop and clang-tidy. Owner, 11 Nov 2026: it runs locally before every PR (`cpp-policy-check .`, result pasted in the PR), not in CI, because the policy repo is private and this fork is public.
- 1.7 `krx_ci.yml`: a Mull job on `ubuntu-latest` over the critical files of `MAINTENANCE.md`, reporting mutation score per function; Mull from a pinned mull-project GitHub release `.deb`, checked by SHA-256 (owner, 11 Nov 2026: Linux only, CI and Docker locally; no Homebrew tap). PR gate: 80 % on the critical lines the PR changes; nightly: every critical line, reported. A nightly 30-minute libFuzzer run (`krx_fuzz`: the first input byte picks the mode, so one run covers all five).
- 1.8 Record the baseline in `test/krx/BASELINE.md`: `krx_bench` p50/p99/p99.9 and throughput for `25273cc3` and `krx/main`, the sanitizer state, the red list (Phase 1 rows; later phases record theirs in their PRs), and the Mull scores once 1.7 runs.
- 1.9 Correct `KRX_FORK_NOTES.md` (still names `krx/fd-double-close`, calls #740 an issue) and turn it into the patch ledger format of `MAINTENANCE.md`.

### Phase 2 — Hostile input never kills the process · **high** · `quickfix`

- 2.1 P0-1: cherry-pick #779 — bounds-check data-length fields against the buffer end, reject negative and oversized values with `InvalidMessage` (`src/C++/Message.cpp:648-670`).
- 2.2 P0-2: reject a SequenceReset only when logged on; before logon log and disconnect (`src/C++/Session.cpp:352-375, 907-908`).
- 2.3 P0-2: catch `std::exception` and `...` in `Session::next(const Message&)`, `processStream` and the thread procedures; log and `disconnect()` (`Session.cpp`, `ThreadedSocketAcceptor.cpp:253-266`, `ThreadedSocketInitiator.cpp:207-238`).
- 2.4 P0-7: `readMessage` returns message / need more / garbage skipped and never dispatches garbage; the parser resynchronises on the next `8=` instead of clearing the buffer (`ThreadedSocketConnection.cpp:170-204`, `Parser.cpp:33-98`).
- 2.5 P1-13: cap the parser buffer (1 MB) and BodyLength; disconnect when exceeded (`Parser.cpp:64-77`).
- 2.6 P1-11: validate ResendRequest `1 ≤ BeginSeqNo ≤ EndSeqNo` (EndSeqNo 0 = infinity) before any store lookup (`Session.cpp:383-519`).
- 2.7 P2-4: check `npos` in resend with `PreserveMessageFieldsOrder` (`Session.cpp:435-441`).

### Phase 3 — Sockets never freeze a session · **high** · `quickfix`

- 3.1 P0-3: non-blocking send with `poll(POLLOUT, SendTimeout)`; default shorter than HeartBtInt; on timeout `shutdown()` the fd and fail the send (`ThreadedSocketConnection.cpp:77-94`, `Utility.cpp`).
- 3.2 P0-3: `SO_KEEPALIVE` on, `TCP_USER_TIMEOUT` ≈ 2×HeartBtInt (Linux) on every session socket; new settings documented in `doc/`.
- 3.3 P0-3: `disconnect()` sets an atomic kill flag and shuts the fd down before it takes `m_mutex`, so a stuck sender is released (`Session.cpp:562-645`).
- 3.4 P0-3 / P1-11: resend in chunks, releasing `m_mutex` between chunks (`Session.cpp:377-381, 383-519`).
- 3.5 P1-3: retry `poll` and `recv` on `EINTR` in `ThreadedSocketConnection::read()` (`:113-154`).
- 3.6 P1-4: the accept loop retries `EINTR`/`ECONNABORTED`, backs off on `EMFILE`, logs every failure (`ThreadedSocketAcceptor.cpp:210-244`).
- 3.7 P1-10 and P1-12: call `next(now)` at most once a second on every read-loop pass; non-blocking connect with a timeout so `SocketConnectHost1` failover is fast (`ThreadedSocketConnection.cpp:129-142`, `ThreadedSocketInitiator.cpp:37-68`, `Utility.cpp:240-268`).
- 3.8 Resend under contention, measured (added 12 Nov 2026, follows 3.4). 3.4 lets threads waiting for `m_mutex` in between resent messages: a wait loop with `std::this_thread::yield()`, capped at 10 ms. No test can see what the yield does, so Mull's mutant removing it survives; the owner accepted it as equivalent until this item measures it (MAINTENANCE §5). Tests first, as for engine code:
  - 3.8a TDD: move the percentile code out of `Bench.cpp` into `LatencyStats.{h,cpp}` in `src/C++/test/krx/`; its rows (3.12) are written and red before the move.
  - 3.8b BDD: `krx_bench resend`: one session resends N stored messages (default 20,000) to a counterparty that reads as fast as it can, while S sender threads (default 2) place orders. It reports the resend time, each sender's `send()` time (p50/p99/p99.9/max), the orders placed during the resend and the process CPU time. The run is a function the scenarios call with small numbers (rows 3.13, 3.14), so it is tested in `krx_ut`; the scenarios are red (the function does not exist) before it is written.
  - 3.8c Measure three handoffs, each an engine build: yield (3.4 as merged), spin (b1a63c0c), a 1 ms sleep when a thread waits (a local patch, not committed). Linux Release in Docker with `--cpus=1`, `2` and `4` (a CPU quota, as in Kubernetes) and pinned to one core with `taskset`; three interleaved runs each, medians into `test/krx/BASELINE.md`.
  - 3.8d Decide with the owner: yield measurably better than spin on 1–2 CPUs → a scenario pinned to one CPU that shows it (kills the mutant); no difference → the mutant stays recorded as equivalent; every variant poor at `--cpus=1` → replace the wait loop with a condition-variable handoff (waiters signal once they hold the lock, no spinning), with its own red test.

### Phase 4 — Store survives disk errors and crashes · **high** · `quickfix` · waits for Q1

- 4.1 P0-4: write body, flush, then header; insert into `m_offsets` only after both succeed (`FileStore.cpp:209-223`).
- 4.2 P0-4: on any write error `clearerr`, `ftruncate` back to the last good offsets and throw a store error; `Session` fails closed (D7) (`FileStore.cpp`, `Session.cpp` persist path).
- 4.3 P0-4: `populateCache` drops header records past the end of the body file (`FileStore.cpp:130-148`).
- 4.4 P0-5 (per Q1): opt-in `FileStoreSync=Y` — `fdatasync` after `set`+`setSeqNum` with group commit; measure p50/p99.9 with it on and off (`FileStore.cpp:220-223, 308-328`, `SessionSettings`).
- 4.5 P2-4: `FileStore::get` keeps embedded NULs (`msg.assign(buffer, size)`, `FileStore.cpp:338-347`).

### Phase 5 — No silent loss · **high** · `quickfix` · waits for Q2, Q3

- 5.1 P0-6 (per Q2): `send()` returns `false` when the message was stored but not written, and when `Responder::send` fails (`Session.cpp:606-637`).
- 5.2 P0-6: a store failure inside `persist` does not leave an unsent message under a consumed seqnum (`Session.cpp:695-702`).
- 5.3 P0-8: count every outbound 35=3 and 35=j and log it at error level with the rejected seqnum and reason, so the gateway can alert (`Session.cpp:847-849, 1324-1338`).
- 5.4 P0-8 (only if Q3 picks the engine patch): deliver a failed application message to `fromApp` with a validation-failed flag instead of consuming it.

### Phase 6 — Race-free teardown and locking · **high** · `quickfix`

- 6.1 P0-11: `setResponder` takes `m_mutex`; all connection teardown goes through `Session::disconnect()`, which also clears `sentLogon` (`Session.h:238-245`, `ThreadedSocketConnection.cpp:65-75`).
- 6.2 P0-11: `m_disconnect` becomes `std::atomic<bool>`. *Already atomic since Phase 3 (P0-3).*
- 6.3 P1-1: cherry-pick #772 (`std::recursive_mutex`), #773, #774, #781 as one set, reviewed together (`Mutex.h`, `SessionState.h:58-150`). *Done early, in Phase 3 (12 Nov 2026): item 3.3's cross-thread `disconnect()` cannot pass TSan without them. #772 in #28; #773, #774 (simplified: no range checks before 1970 or after 2262) and #781 in the next PR. Row 6.3, the threaded acceptance suite under TSan, is still checked in Phase 6.*
- 6.4 P1-2: call `onLogout` after releasing `m_mutex`; document which callbacks still run under the lock (`Session.cpp:581-688`).
- 6.5 P1-7: test-for, then cherry-pick #740 (`onStop` after `thread_join`); check it against fork fix `5d85a415` (`Acceptor.cpp:248-250`).
- 6.6 P2-3: `setSession` failure unregisters; `onStop` spin sleeps between checks (`ThreadedSocketConnection.cpp:222-234`, `ThreadedSocketAcceptor.cpp:141-152`).

### Phase 7 — Upstream cherry-picks and hygiene · **medium** · `quickfix` · 7.3 waits for Q5, 7.4 for Q6

- 7.1 P1-5: cherry-pick #715 (`d5a5cfdd`, GapFill leaves skipped messages in `m_queue`) with its upstream test.
- 7.2 P1-6: cherry-pick #764 (`FieldMap` move assignment).
- 7.3 P1-8: cherry-pick #783 if `NonStopSession=Y` (`SessionFactory.cpp:213-214`).
- 7.4 P2-2: cherry-pick #677, #751, #713 only if built-in TLS is used.
- 7.5 P2-1: cherry-pick `7deb8275` (`string_concat` leak, `Utility.cpp:112`).
- 7.6 P0-8 support: test-for #743 (nested group counts) and #755 (settings applied to the default dictionary); cherry-pick if they fail on the fork.
- 7.7 Every pick gets a ledger line with its upstream URL, and its upstream tests run in the fork.

### Phase 8 — Sequence-gap root cause and long runs · **high** · `quickfix`

- 8.1 P0-10: fix the stress harness so it keeps `Session*` pointers and every session logs back on; rerun `STRESS_SEED=4 STRESS_FILELOG=1 STRESS_NO_ECHO=1 ./run_stress.sh asan 90 4 3 <port>` and trace seq 26173 through the event logs.
- 8.2 P0-10: write the root cause in `KRX_FORK_NOTES.md` (code path, or proof it is a harness artifact); fix test-first if it is a defect.
- 8.3 50+ seeded chaos runs across Release, ASan and TSan: LOST = 0 after settle, every gap a delivered message or a logged GapFill.
- 8.4 TSan stress ≥ 1 h on each of 10 seeds: zero unsuppressed reports.
- 8.5 Echo-on chaos (the deadlock topology) 20 runs with no watchdog hang; Release soak ≥ 8 h with LOST = DUP = OOO = 0 and flat RSS and fd counts.
- 8.6 libFuzzer ≥ 24 CPU-h per mode (including chunked `Parser`) with no crash; stateful session fuzzing over a socketpair (pre-logon, mid-resend, during logout) 24 CPU-h with no crash, no `std::terminate`, bounded memory.
- 8.7 Shutdown under load: `stop()`, `stop(true)` and destroy while threads send — 100 iterations, each exits within 15 s.

### Phase 9 — quickfix-rs fork fails closed · **high** · `gianghn/quickfix-rs`

- 9.1 P0-9: `callback_to_code` maps a panic to an error code, never to success (`quickfix/src/application.rs`, `log_factory.rs:64-92`).
- 9.2 P0-9 (per Q4): the gateway build fails closed on panic; a test injects a panic in `on_msg_from_app` and checks the target seqnum is not advanced (or the process aborts) and the message is recovered by ResendRequest on reconnect.
- 9.3 Compile-time `Send + Sync` assertion on the callback type; no `&Message` kept past a callback.
- 9.4 Trap-3: pin the threaded classes explicitly; vendor `libquickfix` from the `krx-1.0.0` tag, not upstream master.
- 9.5 Make `quickfix/tests/test_session.rs` reliable (unique session IDs per test, `&&` logon wait, no fixed sleeps), then `cargo-mutants` on the FFI callback module.

### Phase 10 — Gateway integration and conformance · **high** · `enCapital/krx_gateway_rust`

- 10.1 P0-6: refuse orders unless `is_logged_on()`; in `toApp`, a PossDup staleness policy — `DoNotSend` (gap fill) for NewOrderSingle/Cancel/Replace older than N s, reported to the OMS as not sent.
- 10.2 ClOrdID journal written before every send; every order idempotent by ClOrdID.
- 10.3 P0-8: production dictionary built as an exact superset of the HOSE/KRX member spec; alert on every outbound 35=3/35=j.
- 10.4 P1-2: callback discipline — callbacks never block and never take a lock held around `send_to_target`; tests for lock inversion, sending from inside callbacks, and many senders while the link drops (ASan).
- 10.5 Conformance: the 59 FIX session test cases replayed through quickfix-rs against a HOSE/KRX simulator configured like production.
- 10.6 Gateway scenarios: reconnect with gaps both ways, kill -9 and restart, EOD/SOD reset, weekend and holiday, logon outside the window, 09:00 resend storm, PossDup resend with ClOrdID dedup, SequenceReset-Reset lower, garbled frame mid-burst, exchange disconnect mid-burst.
- 10.7 KRX schedule: ATO 09:00–09:15, lunch 11:30–13:00 with the session up, ATC 14:30–14:45, put-through to 15:00, periodic call auctions, halt resumption, MTL orders, amend rule.
- 10.8 Pre-trade controls (price collar, ceiling/floor, max qty/value, throttles below the HOSE limit, duplicate detection, foreign room, restricted list) and a two-layer kill switch, each with test evidence.

### Phase 11 — Fault injection and performance · **high** · `enCapital/krx_gateway_rust` + lab

- 11.1 Network faults with `tc netem` and toxiproxy: delay, loss, reorder, duplicate, bandwidth cap, reset-peer, slow-close, zero window.
- 11.2 Process faults: SIGKILL at random points in a burst ×100, SIGSTOP/SIGCONT of the simulator, signal storms on the gateway.
- 11.3 Disk faults: ENOSPC, EIO (dm-flakey), read-only remount of the store volume during trading.
- 11.4 Clock faults: ±30 s, ±5 min, a step across the session end, leap-second smear (P1-9; ops mitigation is chrony slew-only).
- 11.5 VM hard power-off during a burst ×20 (per Q1).
- 11.6 Resource exhaustion: fd limit, memory cgroup, CPU starvation.
- 11.7 Performance method document (open-loop load, HdrHistogram with coordinated-omission correction, wire-to-wire timing) and a baseline on production-like hardware: p50/p99/p99.9/p99.99/max, order → wire and ExecutionReport → `fromApp`, fsync on and off.
- 11.8 Full-day soak at 2× peak message rate; 09:00 ATO and 14:30 ATC burst replay with a simultaneous large resend; nightly p99.9 regression job (≤ 10 %).

### Phase 12 — Fork release and governance · **medium** · `quickfix`

- 12.1 Patch ledger complete: every fork commit maps to a defect ID, a test and an upstream PR or "fork-only".
- 12.2 Production build flags `-O2 -g -fstack-protector-strong -D_FORTIFY_SOURCE=2 -D_GLIBCXX_ASSERTIONS`, full RELRO, PIE; checksec passes; no p99 regression.
- 12.3 Signed tag `krx-1.0.0`; the build is reproducible from the tag.
- 12.4 SBOM listing the quickfix and quickfix-rs SHAs; `cargo audit`/`cargo deny` and OS package scan with no open high or critical CVEs.
- 12.5 Change-control process (SSC 379 Art. 3.1.4/6.5, VNX 01 Art. 30 §3.2a): two-person approval, HOSE notification or re-test assessment for every engine change.
- 12.6 Re-certification trigger policy: any fork patch, quickfix-rs upgrade or Reset/validation/dictionary config change is a material change.
- 12.7 Upstream offer (per Q7) and a private-report path for security issues.

### Phase 13 — Operations, HA, security and compliance · **high** · ops, risk, compliance

Non-code; each item has an owner and evidence filed with the go/no-go pack.

- 13.1 Per-session metrics and dashboards (logged-on, seqnums, gaps, resends, 35=3/35=j by reason, queue depth, store latency, latency histograms).
- 13.2 Alerts within 5 s for session down, gap, resend storm, heartbeat miss, outbound reject, store error, clock offset, SLO breach, throttle hit, blocked sender; each fired in a drill.
- 13.3 Runbooks rehearsed: seqnum repair, store repair, forced reset, failover, kill switch, exchange disconnect, disk full; order-status reconciliation after every reconnect and at EOD.
- 13.4 HA: hot standby, active/passive failover (stale standby, standby ahead, split brain), store replication or shared store, backup host failover, DR drill with power.
- 13.5 Persistence ops: dedicated store volume with 80 % and write-error alerts; Reset policy pinned to the HOSE spec; 10-year retention with a tested restore.
- 13.6 Security: network zones, encryption decision (Q6), acceptor allow-list and connection cap, 0600 logs and stores, tag 554/925 masked, pen test and vulnerability scan.
- 13.7 Regulatory: SSC 379 self-assessment with no open gaps, VNX 01 compliance, RTS 6 / SEC 15c3-5 benchmark mapping, SSC Sep 2025 notice evidence, HOSE incident contact and 24 h template.
- 13.8 P1 items not fixed get a signed risk record (owner, mitigation, review date) (per Q8).

### Phase 14 — Certification and go-live · **high** · owner

- 14.1 HOSE/KRX member connection certification passed on the exact `krx-1.0.0` build.
- 14.2 Dictionary conformance: full UAT replay with zero outbound 35=3.
- 14.3 Go/no-go meeting with this plan's checklist signed by the named approvers (Q8).
- 14.4 Phased rollout: shadow/UAT on production lines, a capped account set on day 1, then a ramp, each with exit criteria.
- 14.5 Rollback drill: previous build deployable, store compatible both ways, seqnum hand-over rehearsed.
- 14.6 Hypercare roster (08:30–15:30, two weeks) and day-1 EOD reconciliation with zero unexplained breaks.

### Phase 15 — Store throughput · **high** · `quickfix` · Q18 decided: 15.6 and 15.7

Added 13 Nov 2026 (Q1: "if the sync costs too much speed, we look at it again"). Profile of
`krx_bench` (Mac M4 Max, one sender, `sample`): mem 240 k, file 64 k, file + sync 25 k msg/s. The
sending thread holds the session lock through `persist`, so the store sets the pace:

- file store: about 65 % of the sending thread is store system calls: 3 `write`s (body, header
  record, seqnums), 2 `lseek`s (`fseek(SEEK_END)` on body and header) and a `rewind` per sent
  message; `rewind` + `fprintf` + `write` of the seqnums per received message (64 % of the reading
  thread's work).
- sync: `fsync` of the body 46 %, the three writes 44 %, the socket 3 %. One sync per message, one
  after the other: throughput can never pass 1 / sync latency.
- macOS `fsync` does not flush the drive cache (`F_FULLFSYNC` does), so the Mac sync numbers are
  neither the real durability nor the real cost; 11.7 measures on the production disk.

Items (the owner's numbering of 13 Nov 2026 in brackets):

- 15.0 `Stress.cpp` clean under clang-tidy (array decay, `const`, the vararg `open`, designated
  initializers). *Already done in Phase 8 (a7532eaa); checked again here.*
- 15.1 [3] Fewer system calls per message, same files, same format, same guarantees: body and header
  record written with `pwrite` at end offsets kept in memory (no `fseek`/`ftell`), the seqnums with
  one `pwrite` at 0 (no `rewind`/`fprintf`/`fflush`), `get` reads with `pread`. A failed write still
  rolls both files back (P0-4); Windows keeps the stdio path. Measured before and after (file and
  sync, three interleaved runs, medians) into `test/krx/BASELINE.md`.
- 15.2 [2] No seqnums write for a sent message the body already records: skipped when the next
  sender seqnum is the last stored message + 1; target changes are still written. On open the next
  sender seqnum is the larger of the seqnums file and the last header record + 1. Risk: a sender
  seqnum set lower on purpose (an admin reset) looks like a stale file; proposal: lowering also
  appends a marker record `N,-1,0` (offset −1, which `populateCache`, old builds included, already
  skips), so the last record tells the truth. Red first; waits for Q18 (15.3 changes what the body
  holds when).
- 15.3 [1a] Spike, then Q18: decoupled group commit. `persist` writes without a sync and `send()`
  returns; a flusher per store syncs everything written so far and only then hands those messages
  to the socket, in seqnum order. Nothing reaches the exchange before it is on disk. Changes what
  `send()` returning `true` means (row 4.5, Q2): written and queued, not yet synced or sent; a sync
  or socket failure after the return must disconnect and log an error. The spike measures the
  throughput and latency (sync batch size, p50/p99.9) and lists the risks.
- 15.4 [1b] Spike, then Q18: shared sync. `persist` writes under the session lock; the sender then
  waits outside the lock for a sync covering its message (one waiter syncs for all), and the synced
  messages go to the socket in seqnum order. `send()` keeps its meaning. Gains only with several
  threads sending on one session: `krx_bench` gets a `senders=N` option. The spike measures 1, 2,
  4 and 8 senders and lists the risks.
- 15.5 Decide with the owner (Q18) from the 15.3 and 15.4 numbers and risks, then build the choice
  red first (rows 15.7–15.11), with 15.2 if it still pays. *Decided 14 Nov 2026 (Q18): neither 15.3
  nor 15.4; 15.6 and 15.7 instead. Rows 15.7–15.11 are dropped with them.*
- 15.6 Background flush, for `FileStoreSync=N` (Q18): `send()` stays as in file mode (no sync); a
  flusher thread per store runs `fdatasync` on the body and seqnums every `FileStoreFlushInterval`
  ms (new setting; proposed default 5), so a host crash loses at most about one interval plus one
  sync of messages instead of the kernel's 30 s. Not `sync_file_range` or kernel writeback tuning:
  they stop at the drive's volatile cache. A failed flush fails closed (error event, disconnect), as
  a failed write does (D7). Spike first, measured on the Rocky pod (speed, per-message latency, the
  real window at 1, 5, 10 ms), then red first (rows 15.12–15.14). Still loses up to one window of
  sent messages on power loss: the crash runbook (Q1) keeps the seqnum fix with HOSE and the order
  reconciliation, for fewer orders.
- 15.7 Batch send, for `FileStoreSync=Y` (Q18): `Session::sendToTargetBatch` (and the quickfix-rs
  binding) stores a batch under one hold of the session lock without a sync, syncs once, then writes
  the batch to the socket in seqnum order, and reports per message whether it went out (the spike
  returns all or none). No extra thread; `send()` keeps its meaning and its per-message sync. The
  gateway drains each session's order queue into one call from one sender thread, never waiting to
  fill a batch, at most `MAX_BATCH` (proposed 64) a call (Phase 10). Red first (rows 15.15–15.17).

**Spike results (13 Nov 2026, U-17; numbers in `test/krx/BASELINE.md`).** Measured on Linux in the
lab cluster (a pod in namespace `krx-bench`, store on a node's consumer NVMe) and on the Mac with
a simulated slow sync. The spike (`spike/v1-p15-group-commit`, local, not for merge) is one
flusher thread per session that syncs the body once for everything written since its last sync,
then writes those messages to the socket in order; 1a returns from `send()` once queued, 1b waits
outside the session lock until its message went out.

- The disk decides everything. Linux `fdatasync` on that NVMe is about 1 ms: today's sync mode does
  **about 1,000–1,400 msg/s**, not 27 k (the Mac's `fsync` does not flush the drive cache). A
  production volume with power-loss protection may sync in tens of µs; 11.7 must measure it.
- **1a (decoupled)**: 93 k–173 k msg/s at 1–16 senders on Linux, about 100× today; about 125
  messages per sync under load. RTT p50 unchanged (one sync per round trip each side, about
  1.5 ms). Risks:
  - `send()` returning `true` means stored and queued, not on disk nor on the wire (changes row 4.5
    and the meaning Q2 chose). A sync or socket failure after the return leaves the message stored
    and unsent; it goes out later only as a PossDup resend, which the gateway refuses (Q2), so the
    gateway needs a per-seqnum "not sent" event to tell the OMS (Phase 10 work).
  - The queue between `send()` and the flusher has no bound: a disk stall grows memory and delays
    every order behind it; it needs a cap and back-pressure (as `MaxQueuedMessages`, Q16).
  - A thread per session: start, stop, `disconnect()`, the responder's lifetime and P0-3's send
    timeout all meet a second thread (the Phase 6 race class; the spike's destructor can still
    call a freed responder). TSan and the teardown rows must cover it.
  - The message log (`onOutgoing`) records a message before it is on the wire.
  - Durability holds: only synced messages reach the socket, so after power loss the exchange has
    seen nothing the store lost.
- **1b (shared)**: `send()` keeps its meaning (on disk and written). Gains only with concurrent
  senders on one session: 1.2 k / 2.8 k / 5.2 k / 10.8 k msg/s at 1 / 4 / 8 / 16 senders, about
  0.7 × senders × today; a single sender gains nothing (slightly slower: a thread hand-off). Risks:
  the same second thread and teardown work as 1a; the gateway must send from many threads per
  session to see any gain (to check with the gateway's design).
- **Rocky Linux 9** (userspace in a pod on the node's kernel and NVMe): the same picture. Sync mode
  about 1,500 msg/s, 96 % of each `send()` in `fdatasync` (613 of 636 µs), the engine's own work
  about 20 µs; 1a 87 k–245 k msg/s; 1b 1.4 k / 3.1 k / 6.1 k / 11.3 k at 1 / 4 / 8 / 16 senders.
  With 8 senders 1a waits 82 % of `send()` for the session lock, so its best case is one sender.
  15.1 on the quiet node: file 138 k → 254 k msg/s, no percentile worse.
- **Batch send (the middle option, spiked 14 Nov 2026)**: `Session::sendToTargetBatch` stores a
  batch under one hold of the session lock without a sync, syncs once, then writes the batch to the
  socket in order; no extra thread. One sender on Rocky: 5.5 k / 18.3 k / 56.8 k msg/s at batches of
  4 / 16 / 64 (3.6× / 11.8× / 36.8× today); every order of a batch is on disk and written when the
  call returns, after 680 / 782 / 980 µs (p50; today 647 µs for one). `send()` keeps its meaning and
  no thread is added, so no teardown work; the costs are a new call in the engine and the
  quickfix-rs binding, a gateway that drains its order queue into one call (never waiting to fill a
  batch, which would add latency), a result that says which orders went out when a socket write
  fails mid-batch (the spike returns all or none), and heartbeats and resends waiting behind a
  batch's sync while it holds the lock (about 1 ms at 64, as one send today).
- Before the batch spike, it read: a batch send API (`sendToTarget` of several messages, one sync) would give a
  single gateway thread 1a's batching with 1b's meaning; it changes the quickfix-rs binding.

### For the owner

- Q1–Q8 are answered (12 Nov 2026, in the table above).
- The KRX dictionary is `quickfix-rs/quickfix-msgkrx/src/FIX44KRX.xml`. HOSE's throttles and
  certification script are still needed for Phases 10, 11 and 14.
- Merge the PRs in order (`agent/v1-p1`, `p2`, …); never merge a phase whose CI matrix is not
  green in Release, ASan+UBSan and TSan.

## Handover notes

### Handover 2 — 12 Nov 2026

**Built:** 2.1 P0-1 forged data length rejected (#19, port of upstream #779); 2.2–2.3 P0-2 an
error while a message is handled disconnects that session, never the process (#20); 2.4 P0-7 a
garbled frame is skipped, never re-delivered, and frames behind it are kept (#21); 2.5 P1-13
BodyLength capped at 1 MiB, buffer at 1 MiB + 64 KiB (#22); 2.6 P1-11 ResendRequest range
checked before the store is read, 2.7 P2-4 a stored message without MsgType is gap-filled (#23).
**Red → green:** rows 2.1–2.15 in `src/C++/test/krx/InboundFramingScenarios.cpp`, plus 17
edge-case rows; every fix row was red first (crash, abort, wrong answer or hang) and is green
in CI on Ubuntu, macOS, Rocky 9, ASan+UBSan and TSan.
**Checks:** `krx_ut` 41/41, `ut` 53/53, FIX.4.4 acceptance in CI; all versions 468/468 on
#21 (framing change); `lint.sh` (fork mode) passes; Mull PR gate 100 % on every PR after its
edge cases; the nightly fuzz ran 11 M inputs on the P0-1 fix without a crash.
**Decisions taken:** 2.5 cap sizes (one constant each); 2.7 gap-fills an unreadable stored
message, as FIX does for any message not resent; skipped garbled frames are not logged; 2.2–2.3
catch `std::exception`, not `...` (a Rust panic is Q4).
**Away from the spec:** row 2.12 dropped "in under 1 ms" (the order of the code guarantees no
store lookup; a timing assertion over sockets would be flaky).
**Left for the owner:** the plan uses P2-4 for two defects (2.7 and 4.5); TSan confirmed P1-1
(Phase 6.3) the first time a test sent from another thread, which the gateway does for every
order; a stored message with a bad checksum still stops a resend (Phase 4); the Mull artifact on
calls taking a temporary string is in MAINTENANCE §5.

### Handover 3 — 12 Nov 2026

**Built:** 3.1 send timeout (#25), 3.2 keepalive and TCP user timeout (#26), 3.3 `disconnect()`
releases a stuck sender (#27), 3.5 `EINTR` in the reader (#30), 3.4 a sender gets in during a
large resend (#31, with `m_mutexWaiters`), 3.6 a failed accept no longer stops the acceptor (#32),
3.7 the session timers run once a second whatever arrives (#33) and connect gives up after 2 s so
the backup host is reached within 5 s (#34), row 3.2's test (#35). Also done early: 6.3 (#28, #29).
**Red → green:** rows 3.1, 3.3–3.11 each red on `krx/main` before their fix, in their PRs (3.4:
the order came after all 2,001 resent messages; 3.6: never accepted; 3.10: no timeout in 8 s;
3.11: still connecting to the primary after 5 s). Row 3.2 was never red: `onStop` already shuts the
connections down, so it is a guard. Extra rows from Mull: resend not slowed with no sender waiting,
a disconnect stops a resend.
**Checks:** no `dod.json` in this repo; per PR: `krx_ut` (65/65 at #35) and `ut` 53/53 on macOS,
`krx_ut` in `ubuntu:24.04` (Docker, as CI's Linux), FIX.4.4 acceptance 59/59 plain and threaded
where the acceptor or resend changed, `lint.sh .` passing. CI (Ubuntu, macOS, Rocky 9, ASan+UBSan,
TSan, Mull) ran green on #31 except Mull; #32–#35 get CI once retargeted to `krx/main`.
**Decisions taken:** resend lets waiters in before each message, up to 10 ms; the yield mutant is
accepted as equivalent (owner) until 3.8; accept retries at once after `EINTR`/`ECONNABORTED`, after
100 ms when out of descriptors; connect timeout 2 s, fixed. No new settings in this phase.
**Away from the spec:** 3.4 is not "in chunks" but before each message (Mull: the chunk test only
had equivalent mutants). `ECONNABORTED` (3.8) has no test: it cannot be provoked on demand.
**Left for the owner:** merge #31–#35 in order; the gateway needs `ReconnectInterval` 1–2 s for a
5 s failover; the SSL acceptor and plain `SocketServer` keep the old accept loop (the gateway runs
neither, and Q6 is no TLS); 3.8 (benchmark) when convenient.

### Handover 4 — 12 Nov 2026

**Built:** 4.1 and the store half of 4.2, a failed write leaves the store as it was (#36); the
session half of 4.2, a store that cannot write disconnects the session (D7, #37); 4.3, a restart
loads only records whose message is whole (#38); 4.5, embedded NULs come back (#39); 4.4,
`FileStoreSync` (Q1, #40). Test helpers: `FaultyFile.h` (`RLIMIT_FSIZE` as a full disk, temp
dirs), `PairOptions.file_store_dir` / `file_store_sync`.
**Red → green:** rows 4.1–4.6 each red first, in their PRs (4.1: every read failed after a failed
write; 4.3: still connected; 4.4: the range read failed; 4.5: 0 syncs with the stub; 4.6: 19 of 29
bytes). Extra rows: a failed header write, a store with one message reopened (kills a boundary
mutant, checked by hand), sync off, the setting.
**Checks:** no `dod.json`; per PR: `krx_ut` (75/75 at #40) and `ut` 53/53 on macOS, `krx_ut` in
`ubuntu:24.04`, `lint.sh .` passing; threaded FIX.4.4 acceptance 59/59 at #37. CI runs once
the stack is retargeted to `krx/main`.
**Measured:** the store change (#36): p99.9 +3.3 %, within the 10 % gate. `FileStoreSync=Y` on a
Mac: #40 (three syncs per sent message, one per received) 66,935 → 12,677 msg/s, RTT p99.9
118 → 372 µs; option B (#41, one sync per sent message, none per received) 27,124 msg/s, RTT
p50 102.8 µs, p99.9 195.1 µs (`test/krx/BASELINE.md`).
**Decisions taken:** roll back by close, `truncate`, reopen; a dropped record on restart is not
logged; option B (owner): only the body is synced, the header and the next sender seqnum are
rebuilt from it on open, and a message cut short at its end is removed; a next sender seqnum the
body cannot rebuild (PersistMessages=N, a reset) is still synced; `FileStoreSync` default N;
`FileStore::getSyncCount()` added so a test (and operations) can see the syncs.
**Away from the spec:** "group commit" across messages is not needed: sends are serialised by
the session lock, so each message is its own commit. The next target seqnum is not synced: after
a crash it can be behind, and the exchange resends (PossDup); the gateway must drop duplicates by
ExecID (Phase 10).
**Left for the owner:** the Q1 crash runbook (reconcile seqnums with HOSE after a crash) is not
written: it needs HOSE's procedure, best in Phase 13 with operations. The sync cost on the
production disk (11.7) decides whether `FileStoreSync=Y` goes live; option C (sync every N ms, a
setting) is the step after B if it is still too slow.

### Handover 5 — 12 Nov 2026

**Built:** 5.1 `send()` returns `false` when the order was not written: before Logon, or a write
that fails (Q2, #42); 5.2 a seqnum write that fails after the order was stored puts the seqnum
back, so the stored copy is replaced and never resent (#43); 5.3 every outgoing Reject and
BusinessMessageReject is logged as `ERROR: sent <type> for RefSeqNum <n>: <reason>` (Q3, #44).
5.4 not built: Q3 chose the full dictionary plus alerts. Test fixture: `SeqNumFailingStore`.
**Red → green:** rows 5.1–5.4 each red first, in their PRs (5.1, 5.2: `true`; 5.3: the stored copy
was the failed order; 5.4: no error event). Extra row: an outgoing 35=j is logged too.
**Checks:** no `dod.json`; per PR: `krx_ut` (84/84 at #44) and `ut` 53/53 on macOS, `krx_ut` in
`ubuntu:24.04`, FIX.4.4 acceptance 59/59 plain and threaded, `lint.sh .` passing. CI runs once
the stack is retargeted to `krx/main`.
**Decisions taken:** admin messages also report a failed write; a 35=j the gateway itself sends is
logged as an error too ("every outbound", row 5.4); the error is one event with a fixed prefix,
`ERROR: sent`, since `FIX::Log` has no levels.
**Away from the spec:** "count every outbound 35=3 and 35=j" is the log event, not a counter.
**Left for the owner and the gateway (Phase 10):** the gateway refuses PossDup orders in `toApp`
(Q2), drops duplicate ExecIDs (option B), alerts on `ERROR: sent` events (Q3), and its
dictionary covers `FIX44KRX.xml` (Q3).

### Handover 6 — 13 Nov 2026

**Built:** 6.1 a connection that closed itself now ends its session through `disconnect()`
(`Session::releaseResponder`, called from `read()`), `setResponder` takes the session lock, and
the initiator attaches a connection only once connected (#46); 6.2 was already done (atomic
`m_disconnect`, Phase 3); 6.3 done early (#28, #29), its row now checked: the threaded
acceptance suite runs under TSan in CI (#50); 6.4 `onLogout` runs after `disconnect()` releases
the session lock (#47); 6.5 `Acceptor::stop` joins the `onStart` thread before `onStop`
(quickfix/quickfix#740, #48); 6.6 a connection never takes a session of another port, and
`ThreadedSocketAcceptor::onStop` sleeps while it waits for logouts (#49). Test fixtures:
`SessionPair::DropConnection`, `LoggedOn`, `ProbeDuringLogout`; `StartedAcceptor` and
`Counterparty` in `TeardownRaceScenarios.cpp`.
**Red → green:** 6.1 TSan data race (`Session::send` against `setResponder` in the connection's
destructor) → none; 6.2 `onLogout` never called → called; 6.3 59/59 threaded acceptance with no
TSan report (Docker; a guard: no red); 6.4 a send from another thread blocked on the session lock
during `onLogout` → returns; 6.5 35–37 of 40 starts failed and ~40 descriptors leaked → none;
6.6 the wrong port reached the session and its own-port Logon went unanswered → refused, logs on;
a forced stop used 2998 ms CPU in 2998 ms → under 500 ms.
**Checks:** no `dod.json`; per PR: `krx_ut` (89 → 92 cases) and `ut` on macOS, `krx_ut` under TSan
in `ubuntu:24.04` (twice), FIX.4.4 acceptance 59/59 plain and threaded, `lint.sh .` passing. #46
green in CI (mutation 100 %); #47–#50 get CI once retargeted to `krx/main`. Local ASan in Docker
is unusable today (as slow on `krx/main`): CI's `sanitize (address)` is the ASan check.
**Decisions taken:** `onLogout` comes after the session reset; on a self-closed connection it
runs in the connection's thread, outside the acceptor's and initiator's locks. Callbacks still
under the session lock are listed in `KRX_FORK_NOTES.md` (P1-2): `toAdmin`, `toApp`, and
`onLogout` when the caller holds the lock (store failure in `sendRaw`, a resend, the reset outside
session time). The initiator's `stop()` order (6.5) and its `onStop` spin (6.6) are unchanged: no
test made them fail; Phase 8.7 (shutdown under load) takes them, red first.
**Away from the spec:** none.
**Left for the owner:** merge #45 (nightly mutation fix; scores are 50–51 %, the job now takes over
2 of its 3 hours), then the Phase 6 stack #46–#50 in order, each retargeted and green first.
*Merged 13 Nov 2026 (#45–#50), each green in CI on the `krx/main` it merged into.*

### Handover 7 — 13 Nov 2026

**Built:** 7.1 a GapFill drops the queued messages it skips and logs `GapFill to N dropped K queued
messages` (#715, #51); 7.2 `FieldMap` self-assignment keeps the map, move assignment frees the
replaced groups (#764, #52); 7.5 `findCAList` frees each CA path (`7deb8275`, TLS builds only,
#53); 7.6 nested repeating group counts are validated (our check, after #743, #54) and validation
settings apply with `UseDataDictionary=N` (#755, #55); 7.7 a ledger line per pick and every
upstream test in `ut`. 7.3 and 7.4 not built (Q5: no `NonStopSession`; Q6: no TLS). Scenarios in
`UpstreamPicksScenarios.cpp`.
**Red → green:** 7.1 no event (the skipped messages stayed queued) → event; 7.2 self-assignment
kept nothing and the replaced group was never freed → kept, freed; 7.6 802=2 with one entry passed
→ `RepeatingGroupCountMismatch`, and `ValidateFieldsHaveValues=N` still refused an empty field →
accepted. Upstream tests: #764's three, #743's and #755's red, then green; #715's two already
passed (the fork drained the queue). 7.5 has no test (no TLS build).
**Checks:** no `dod.json`; per PR: `krx_ut` (98 cases at #55) and `ut` (53 cases, 1974
assertions) on macOS, FIX.4.4 acceptance 59/59 plain and threaded, `lint.sh .` passing; TSan
`krx_ut` (7.1) and ASan+LSan on the 7.1/7.2 scenarios in `ubuntu:24.04`. CI runs as each PR is
retargeted to `krx/main`.
**Decisions taken:** 7.1 adds the event, which upstream lacks (without it no test sees the fix;
the counterparty gap-filling messages it sent is worth an alert) and leaves out #715's redundant
`nextQueued`; 7.6 checks nested counts with our own code, since #743's code refuses valid messages
here; the four validation setting names became `constexpr const char*` (house lint).
**Away from the spec:** none.
**Left for the owner:** merge #51–#55 in order (each retargeted and green first). Next: Phase 8.

### Handover 15 — 14 Nov 2026

Branch `agent/v1-p15a` (from `agent/v1-p8e`, merged as #60; merges cleanly into `krx/main`). Not
pushed, no PR yet. Numbers: `test/krx/BASELINE.md`; rows: `docs/V1_BDD_CHECKLIST.md` §15.

**Built:**
- 15.0 `Stress.cpp` clean under clang-tidy: already done in Phase 8 (a7532eaa), checked again.
- 15.1 (90665b83) fewer system calls per stored message, same files and format: body and header
  record are one `pwrite` each at end offsets kept in memory (`m_msgEnd`, `m_headerEnd`, set in
  `open()` after recovery); seqnums one `pwrite` at 0; Windows keeps the stdio path.
- 15.6 (78cab7d4) background flush for `FileStoreSync=N`. New setting `FileStoreFlushInterval`
  (ms; 0 or absent = off; ignored when `FileStoreSync=Y`); new `FileStore` constructor parameter
  `flushIntervalMs = 0`. One flusher thread per store, started on the first write, `fdatasync`s
  body and seqnums on a fixed tick when anything was written (it syncs `dup`ed descriptors taken
  under `m_flushMutex`, so a write never waits for a sync). A failed flush makes the next `set()`
  throw, so the send fails closed (D7). The flusher syncs through the protected `setFlushSync`
  (a plain function, default `fdatasync`; the tests' failing-disk seam), never the virtual
  `syncFile`, so it cannot call into a subclass being destroyed. `syncFile` stays the seam for the
  sync-mode writes (15.7's `SyncFailingFileStore`).
- 15.7 (e3374ac0) batch send for `FileStoreSync=Y`: `std::size_t Session::send( std::vector<Message>& )`
  and static `Session::sendToTarget( std::vector<Message>&, const SessionID& )`. Under one hold of
  the session lock: `store->beginBatch()`, each message stored without a sync, `endBatch()` syncs
  the body once, then the batch goes to the socket in seqnum order. Returns how many went out,
  from the first; a refused message ends the batch there. A failed batch sync logs "Store write
  failed, disconnecting: unable to sync a batch to disk", disconnects and returns 0.
  `MessageStore::beginBatch()`/`endBatch()` are new virtuals that do nothing by default.
- `krx_bench` arguments: `senders` [6], `flush_ms` [7], `batch` [8]. Tests:
  `src/C++/test/krx/StoreThroughputScenarios.cpp`; `SessionPair` gained `SyncFailingFileStore`,
  `PairOptions.sync_fails` and `SendBatch`.

**Production (owner, 14 Nov 2026): background flush.** The gateway runs `FileStoreSync=N` with
`FileStoreFlushInterval=5` and keeps calling `send()` as today; a host crash or power cut can lose
up to about 10 ms of sent orders, recovered by the Q1 runbook (seqnum fix with HOSE, order
reconciliation; signed risk record, 13.8). The batch send (15.7) stays in the library for sync
mode; the quickfix-rs binding and the gateway's batch sender loop (Phases 9–10) are not needed for
production unless sync mode comes back. With sync on, one sender thread per session would drain the
order queue into one `sendToTarget(vector, id)` call, never waiting to fill a batch, at most 64 a
call; orders after the returned count were not sent.

**Red → green:** 15.1: 0 → 3 (guards, green before and after; evidence U-16). 15.6: 3 → 3 (rows
15.12–15.14). 15.7: 3 → 3 (rows 15.15–15.17).
**Checks:** `krx_ut` 118 cases and `ut` 53 pass (Release, Mac); TSan (Mac) clean on 25 store and
send scenarios; `lint.sh .` passes. Linux ASan/UBSan, TSan and Mull run in CI only, not yet run.
**Measured (Rocky 9 userspace in lab pods, ext4 on consumer NVMe):**
- Today's sync mode: about 1.1–1.5 k msg/s, 96 % of each send in `fdatasync` (about 0.6 ms; the
  Mac's 27 k was a cache-only `fsync`).
- 15.1, file mode: 138 k → 254 k msg/s, no percentile worse.
- 15.7, sync on: batch 4 / 16 / 64 = 3.7 k / 11.8 k / 32.2 k msg/s (3.4× / 10.6× / 29×), call p50
  0.9–1.4 ms (node3, busy).
- 15.6: flush off costs nothing (D9 holds). Flush 5 ms: 0 % (node2) to 4.5 % (node3) slower;
  messages at risk on a host crash p50 7.7 / p99 9.2 ms (was up to the kernel's ~30 s). An earlier
  −25 % on node3 came with three soak jobs running, not from the flush.

**Measured, not built:** 15.3 (1a: `send()` returns before the sync) and 15.4 (1b: one sync shared by
sender threads), spikes on local branch `spike/v1-p15-group-commit`; owner chose 15.6 and 15.7
instead (Q18, rows 15.7–15.11 dropped). Pre-sizing the body file (`fallocate` or zeros), spike on
local branch `spike/v1-p15-presize`: no gain (`fallocate` within the noise, zeros 30–37 % slower);
look again only if 11.7 shows flush stalls on the production disk. Both spike branches fail tidy on
purpose and are never merged.

**Decisions taken:** Phase 15 numbered after 14; bench pods in their own namespace `krx-bench`
(owner, 13 Nov 2026; quota 6 CPU, request 150m); Windows keeps stdio writes; a batch result is a
count from the first; a refused message ends the batch; the batch's wire text is logged when
written; a failed flush fails the next `set()` rather than the session at once (the store cannot
reach the session); the batch hooks default to nothing, so other stores (quickfix-rs's included)
need no change.
**Away from the spec:** 15.7 is an overload `sendToTarget(vector, id)` returning a count, not
`sendToTargetBatch` with a per-message result. 15.6 is off by default, not 5 ms: opt-in per D8.
**Left for the owner:**
- Merge the three stacked PRs in order: `agent/v1-p15a` (15.1 and the plan), `agent/v1-p15b`
  (15.6), `agent/v1-p15c` (15.7 and the measurements).
- This decision was Q17 until 14 Nov 2026; renumbered Q18 (owner) because open PR #65
  (`agent/v1-p8i`) took Q17 for the resend range. `docs/V1_PLAN.md` conflicts with #62 and #65:
  whichever merges second resolves it (both add rows to Decisions, Progress and Handover notes).
- 15.2 (skip the seqnums write the body already records): still open; small gain in file mode, some
  risk around admin resets; proposal: drop it.
- 11.7 on the production disk (likely XFS): sync cost and flush cost decide the batch cap and
  interval.
- Production uses background flush: Phases 9–10 need only the `FileStoreFlushInterval` setting
  passed through; the batch-send binding and sender loop wait unless sync mode comes back.
- Cleanup: `kubectl delete ns krx-bench`; delete the two local spike branches.
