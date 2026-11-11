# Maintaining the KRX fork

This fork (`gianghn/quickfix`, branch `krx/main`) is the FIX engine under DNSE's HOSE/KRX trade
gateway. Upstream has merged nothing since 21 May 2026, so we own it: every defect that reaches
production is ours. This guide is how any change gets into `krx/main`. The current release plan
is [V1_PLAN.md](V1_PLAN.md); the fix ledger is [`KRX_FORK_NOTES.md`](../KRX_FORK_NOTES.md).

## 1. Rules that never bend

1. **Test first.** No engine change without a test that fails before it. Record the red run.
2. **Critical functions get the full treatment** (section 4): edge-case table, sanitizer matrix
   and a mutation run.
3. **Fail closed.** When the engine cannot do what it was asked (write the store, send the
   bytes), it says so: return `false`, throw, or disconnect. Never report success for work not
   done.
4. **No crash on input.** Bytes from the network can make the engine reject, disconnect or log;
   they must never end the process.
5. **No blocking under `Session::m_mutex`** without a timeout.
6. **Every change is a material change** for HOSE (V1 plan, 12.6): ledger line, two approvers,
   re-certification assessment.
7. **No suppressions** of any kind, sanitizer suppression files included (house policy). Fix the
   code.

## 2. How a change lands

1. Give it an ID: a defect (`P0-3`, or the next `K-<n>` for a new one) or an upstream pick
   (`#779`).
2. Write the scenario (section 3) and run it on `krx/main`. It must fail for the reason you
   expect. Paste the failure into the PR.
3. Make the smallest fix that turns it green. Keep public method signatures: quickfix-rs binds
   them.
4. If the change touches a critical function: update its edge-case table, add the missing
   cases, run Mull on the changed functions (section 5).
5. Run the CI matrix locally or on the PR: Release, ASan+UBSan, TSan × unit, acceptance,
   threaded acceptance. All green.
6. If the change is on the send, store or socket path, run `test/krx/bench` before and after;
   p99.9 may not regress by more than 10 %.
7. Add a ledger line to `KRX_FORK_NOTES.md` (section 8).
8. One PR per phase (or per fix outside a release), reviewed by a second engineer.

## 3. Tests: where and how

| Level | Where | Runs |
|---|---|---|
| Upstream unit tests | `src/C++/test/*TestCase.cpp` | `cd test && ./ut --quickfix-config-file cfg/ut.cfg --quickfix-spec-path ../spec` |
| Our scenarios | `src/C++/test/krx/*Scenarios.cpp` (Catch2 `SCENARIO`/`GIVEN`/`WHEN`/`THEN`, target `krx_ut`) | `krx_ut` (all), `krx_ut "[p0-3]"` (one defect), or `ctest` in the build folder |
| FIX acceptance | `test/definitions/server/fix4*/` | `cd test && ./runat.sh 6666`, `./runat_threaded.sh 6666` |
| Seed replay | every I8/E8 message of the acceptance `.def` files, through the five fuzz modes (`FixInput`) | part of `krx_ut` |
| Stress and chaos | `test/krx/stress.cpp`, `run_stress.sh` | `run_stress.sh <build> <seconds> <threads> <sessions> <port>`; `STRESS_SEED`, `STRESS_NO_ECHO`, `STRESS_FILELOG` |
| Fuzzing | `test/krx/fuzz_message.cpp` (5 modes, incl. chunked `Parser`) | nightly 30 min in CI; 24 CPU-h per mode before a release |
| Benchmark | `test/krx/bench.cpp` | before and after any send, store or socket change |
| quickfix-rs | `gianghn/quickfix-rs` `quickfix/tests/` | `cargo test`; `cargo-mutants` on the callback module |
| Gateway | `krx_gateway_rust/tests/features/` | its own CI and definition of done |

Writing a scenario:

- Name the behaviour, not the code: `SCENARIO("A SequenceReset before Logon ends the connection
  and the process keeps running", "[krx][p0-2]")`.
- Drive a real `Session` through the socketpair fixture (`SessionPair.h`) rather than mocking
  it; the defects we fix live in the interaction between session, socket and store.
- Put comparisons in double parentheses, `REQUIRE((reply == "A"))`, with an `INFO()` line for the
  values: the house clang-tidy reads Catch2's `REQUIRE(a == b)` as a chained comparison
  (`bugprone-chained-comparison`), and the double parentheses lose Catch2's own value printout.
- No sleeps to wait for something; wait on a condition with a deadline.
- Each scenario uses its own SessionID and store directory, so scenarios can run in parallel
  (quickfix-rs's `test_session.rs` crashed because two tests shared one).
- Fault injection uses the OS: `RLIMIT_FSIZE` for ENOSPC, a read-only directory for EACCES, a peer
  that never reads for zero window, `pthread_kill` for EINTR.

## 4. Critical functions

A bug in these loses orders, freezes the session or crashes the process. Every change to one of
them needs its edge-case table (below, kept up to date in this file) fully covered, all three
sanitizers green, and a Mull run.

| Area | Functions (`src/C++/`) |
|---|---|
| Framing and parsing | `Parser::extractLength`, `Parser::readFixMessage`, `Message::setString`, `Message::extractField`, `ThreadedSocketConnection::readMessage`, `ThreadedSocketConnection::processStream` |
| Session state machine | `Session::next(const Message&…)`, `Session::next(const UtcTimeStamp&)`, `Session::verify`, `Session::nextLogon`, `Session::nextSequenceReset`, `Session::nextResendRequest`, `Session::doTargetTooHigh`, `Session::nextQueued`, `Session::generateReject` |
| Sending | `Session::sendRaw`, `Session::send`, `Session::sendToTarget`, `Session::persist`, `Session::resend`, `Session::disconnect`, `ThreadedSocketConnection::send` |
| Store | `FileStore::set`, `FileStore::get` (both), `FileStore::populateCache`, `FileStore::setSeqNum`, `FileStore::incrNext*MsgSeqNum`, `FileStore::reset` |
| Socket threads | `ThreadedSocketConnection::read`, `::setSession`, `ThreadedSocketAcceptor::socketAcceptorThread`, `::socketConnectionThread`, `::onStop`, `::releaseConnection`, `ThreadedSocketInitiator::socketThread`, `::doConnect`, `::onStop` |
| Shared state | `Session::setResponder`, `SessionState` flag accessors, `FIX::Mutex` (until #772 replaces it) |

### Edge-case tables

Each table lists the classes of input every change must still be tested against. Add a row when
a new class shows up (from a bug, a fuzz finding or a surviving mutant).

**Framing and parsing**

| Class | Cases |
|---|---|
| Lengths | BodyLength 0, 1, exact, one short, one long, `INT_MAX`, negative, non-numeric (`9=abc`), leading zeros, missing |
| Data-length fields (95, 90, 354, 212, 348…) | 0, exact, past buffer end, negative, 1e8, 2e9, missing its data field |
| Checksum | correct, wrong, missing, not three digits, `10=` not last |
| Framing | two messages in one read, one message across many reads (1 byte at a time), garbage before `8=`, garbage between messages, a truncated message followed by a good one, buffer at the cap and one byte over |
| Content | embedded SOH in data fields, embedded NUL, duplicate tags, tag 0, tag > 2^31, empty value, repeating group count 0 / wrong / nested |

**Session state machine**

| Class | Cases |
|---|---|
| State | before Logon, during Logon, logged on, during Logout, after Logout, after disconnect |
| Seqnums | expected, too high (gap), too low with and without PossDup, equal to 1 after reset, `INT_MAX` |
| SequenceReset | GapFill up, GapFill down, Reset up, Reset down (NewSeqNo < expected), before Logon |
| ResendRequest | begin 1, begin = end, end 0 (infinity), begin > end, negative, beyond next sender seqnum, during an ongoing resend |
| Errors | `fromApp` throws a FIX exception, a `std::exception`, anything else; dictionary rejects an app message; clock step across the session end |

**Sending and store**

| Class | Cases |
|---|---|
| Link | logged on, not logged on, socket closed mid-send, partial send, EINTR before and after the first byte, peer at zero window, send timeout |
| Store | write succeeds, header write fails, body write fails, flush fails, disk full then freed, read-only, restart after each failure, embedded NUL, record past end of body file, sync on and off |
| Concurrency | many senders, a sender during disconnect, a sender during resend, a callback that sends, `stop()` and destroy while sending |

**Socket threads**

| Class | Cases |
|---|---|
| Accept | EINTR, ECONNABORTED, EMFILE, RST during handshake, connection that never logs on |
| Read | EINTR in `poll` and `recv`, peer trickling bytes, POLLHUP, peer closes mid-message |
| Teardown | graceful stop, forced stop, stop during logout callback, repeated stop/start, fd reuse after close (no foreign-file close) |

## 5. Mutation testing

Line coverage says a line ran; a mutation run says a test would notice if it were wrong.

- **Tool:** Mull 0.35.0 for LLVM 18 (pinned `.deb`, SHA-256 checked; Linux only) on a clang-18
  build with `-O0 -g -grecord-command-line -fpass-plugin=/usr/lib/mull-ir-frontend-18` and a
  static engine (`-DQUICKFIX_SHARED_LIBS=OFF`: Mull only sees code inside the test binary).
  `mull.yml` lists the critical files of section 4. Run
  `mull-runner-18 --test-program test/krx/mutation-tests.sh <krx_ut or ut>`: the script runs both
  suites (each with a time limit, so a looping mutant counts as killed), and Mull reads the
  mutants from each binary in turn (each links only the engine code it uses). The CI job
  `mutation` does exactly this. quickfix-rs uses `cargo mutants -f quickfix/src/application.rs`.
- **When:** on every PR, only the critical lines it changes (`gitDiffRef` in `mull.yml`, set by
  the CI job), gated at 80 %; nightly over every critical line, reported; a full run before each
  release tag.
- **Gate:** mutation score ≥ 80 % on each changed critical function. Every surviving mutant is
  either killed by a new test (usually a missing edge case: add it to the table too) or written
  in the PR as equivalent with a one-line reason. Never lower the gate to pass.
- **Mutators:** Mull's defaults plus `cxx_remove_void_call` (drops `fflush`, `clearerr`, unlock
  calls) and boundary mutators (`<` ↔ `<=`): those are where our P0s were.
- Record scores per function in `test/krx/BASELINE.md`; a score may not drop between releases.

## 6. Sanitizers, static analysis and fuzzing

The house C++ policy (Local_Claude_Setup `config/cpp/`) runs in **fork mode** here (`.cpp-policy-fork`):
files that exist at upstream `94625580` are checked only on the lines we change, written in upstream's
style; files we add (`src/C++/test/krx/`, `test/krx/`) get every gate, and their CMake targets go through
`house_target()`. `Local_Claude_Setup/scripts/lint.sh .` must pass before any PR. Never reformat or
modernise upstream lines you are not fixing.

| Check | Gate |
|---|---|
| ASan+UBSan (CI job `sanitize`, address) | `ut`, `krx_ut`, FIX.4.4 acceptance and threaded acceptance green, LeakSanitizer on, no suppressions; the first report fails the job |
| TSan (CI job `sanitize`, thread) | `ut`, `krx_ut` and FIX.4.4 acceptance green with no report; threaded acceptance joins after Phase 6 (row 6.3); 1 h stress × 10 seeds before a release |
| Other FIX versions | CI runs FIX.4.4 acceptance only, in every job (owner, 11 Nov 2026); all versions (`runat.sh`, `runat_threaded.sh`) run before a release and once CI for them is added back |
| Locally | `docker run` an `ubuntu:24.04` with the CI's flags (LeakSanitizer doesn't run on Apple Silicon) |
| House C++ policy (fork mode) | `Local_Claude_Setup/scripts/cpp-policy-check .` passes before every PR, locally (the policy repo is private, this fork public); paste the stage table in the PR |
| libFuzzer | nightly 30 min per mode; 24 CPU-h per mode before a release; every crash becomes a corpus file and a scenario |
| Chaos stress | 50 seeded runs before a release, LOST = DUP = OOO = 0 |
| Soak | ≥ 8 h Release before a release, flat RSS and fd counts |

CI runs on GitHub-hosted runners (the repo is a public fork). Runs longer than a CI job allows are
run in the lab; their logs are attached to the release PR.

## 7. Upstream

- Base stays on upstream `94625580` plus our ledger until a deliberate rebase. A rebase is its
  own release with full re-certification.
- Bring upstream fixes in as single cherry-picks with their upstream tests.
- Traps: never take `b80ed6ff` (#623 reformat) without `b70b7e91` (#685); `681af1fc` (#599)
  changes the `.seqnums` format — test an old store mid-day before any rebase past it;
  quickfix-rs v0.2.2 switches to plain `SocketAcceptor` — pin the threaded classes.
- Check upstream monthly (open PRs, merged commits, SECURITY.md advisories) and note what was
  seen in `KRX_FORK_NOTES.md`.
- Security issues found here are reported to upstream privately (SECURITY.md), never as public
  issues first.

## 8. Ledger and releases

Each `KRX_FORK_NOTES.md` entry has: ID, symptom, cause, how it was found, fix commit, tests
(scenario names and checklist rows), upstream status (PR link, "fork-only", or merged at SHA),
mutation score of the changed functions.

Releases are signed tags `krx-<major>.<minor>.<patch>`. A release needs: CI green on the tag, the
long runs of section 6, an SBOM with the quickfix and quickfix-rs SHAs, the bench compared with the
previous release, and the HOSE re-certification assessment. The gateway pins both forks by SHA.

## 9. Incidents

A production defect gets a `K-<n>` ID, a scenario that reproduces it, and a ledger line before
its fix ships, even as a hotfix. Its edge case goes into the table in section 4 so it cannot
come back unnoticed.
