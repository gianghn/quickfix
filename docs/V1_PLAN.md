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
| 3 | Sockets never freeze a session | 3.1–3.7 | P0-3, P1-3, P1-4, P1-10, P1-12 | in progress: 3.1 (#25), 3.2 (#26), 3.3 (#27), 3.5 (#30), 3.4 (#31), 3.6 (#32), 3.7 P1-10 (#33), 3.7 P1-12 (#34); row 3.2's test and the handover to do |
| 4 | Store survives disk errors and crashes | 4.1–4.5 | P0-4, P0-5, P2-4 | not started |
| 5 | No silent loss | 5.1–5.4 | P0-6, P0-8 | not started |
| 6 | Race-free teardown and locking | 6.1–6.6 | P0-11, P1-1, P1-2, P1-7, P2-3 | not started |
| 7 | Upstream cherry-picks and hygiene | 7.1–7.7 | P1-5, P1-6, P1-8, P2-1, P2-2 | not started |
| 8 | Sequence-gap root cause and long runs | 8.1–8.7 | P0-10, concurrency and fuzzing groups | not started |
| 9 | quickfix-rs fork fails closed | 9.1–9.5 | P0-9, Trap-3 | not started |
| 10 | Gateway integration and conformance | 10.1–10.8 | P0-6, P0-8, P1-2, conformance group | not started |
| 11 | Fault injection and performance | 11.1–11.8 | fault and performance groups | not started |
| 12 | Fork release and governance | 12.1–12.7 | governance, build hardening | not started |
| 13 | Operations, HA, security and compliance | 13.1–13.8 | ops, HA, security, regulatory groups | not started |
| 14 | Certification and go-live | 14.1–14.6 | certification, go-live groups | not started |

### Next steps (12 Nov 2026)

1. **Owner:** merge the Phase 3 stack in order: #31 (3.4; its Mull job fails on the accepted yield mutant, see 3.8), then #32 (3.6), #33 (3.7, P1-10) and #34 (3.7, P1-12), each retargeted to `krx/main` and green first.
2. ~~3.7 P1-12~~ done in #34 (row 3.11). The gateway needs a short `ReconnectInterval` (1–2 s) for a 5 s failover.
3. Row 3.2: `stop()` returns while the exchange reads nothing, no thread left in send (no test yet).
4. 3.8: the resend-under-contention benchmark, then the yield decision with the owner.
5. Phase 3 handover note; Progress row to done.
6. Phase 6 (6.1, 6.2, 6.4–6.6; 6.3 is done) and Phase 8: no owner question blocks them.
7. Q1–Q8 answered (12 Nov 2026), so no owner question blocks a phase now: Phase 4 (sync plus runbook) and Phase 5 can start after Phase 3. The gateway has 4–8 cores and no CPU limit, so 3.8 is not urgent. The KRX dictionary is `quickfix-rs/quickfix-msgkrx/src/FIX44KRX.xml`; HOSE throttles and the certification script are still to be located (Phases 10, 11, 14).

## Phases

Independence: Phase 1 first. Phases 2, 3, 4 and 7 are independent of each other once Phase 1 is
in (stack them in this order to keep the merges simple). Phase 5 needs 3 (send path). Phase 6
needs 3. Phase 8 needs 2–7. Phase 9 can start any time. Phase 10 needs 5 and 9. Phase 11 needs 8
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
- 6.2 P0-11: `m_disconnect` becomes `std::atomic<bool>`.
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
