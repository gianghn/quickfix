# KRX fork notes

This fork (`gianghn/quickfix`, branch `krx/main`) carries fixes the KRX gateway
(`enCapital/krx_gateway_rust`) needs and upstream `quickfix/quickfix` has not merged. It is
based on upstream commit `94625580` (2024-07-24), the one quickfix-rs vendored. The release plan
is [docs/V1_PLAN.md](docs/V1_PLAN.md); how changes land is [docs/MAINTENANCE.md](docs/MAINTENANCE.md).
Add a ledger line with every change, and update it when a fix is sent upstream or merged there.

## Which classes the gateway runs

The gateway asks quickfix-rs for `FixSocketServerKind::SingleThreaded`. Our quickfix-rs passes
`is_single_threaded()` into the C++ flag `isMultiThreaded`, so that builds
**`ThreadedSocketAcceptor` / `ThreadedSocketInitiator`**. Upstream quickfix-rs fixed the swap
(arthurlm/quickfix-rs#36, 2026-10-04). Taking that fix would move the gateway to the plain
`SocketAcceptor` (Trap-3 in the plan): K-6 fixed its triple close, the race below remains, so
don't take it without fixing that and re-running the gateway's checks.

## Ledger

Every fork change that touches engine or test code. Tests are `krx_ut` scenarios
(`src/C++/test/krx/`, rows of [docs/V1_BDD_CHECKLIST.md](docs/V1_BDD_CHECKLIST.md)) unless named.
Mutation scores come with the Mull job (plan item 1.7).

| ID | Defect or change | Fix | Tests | Upstream |
|---|---|---|---|---|
| K-1 | A threaded connection's socket was closed twice (details below) | 5d85a415 | gateway `test_fd_double_close.rs`, `fix_connection_safety.feature` rule 1 | fork-only (no issue or PR, checked 2026-11-09) |
| K-2 | A partial send put bytes from past the message on the wire (details below) | 0efe36b8 | gateway `test_partial_send.rs`, `fix_connection_safety.feature` rule 2 | quickfix/quickfix#742 (open) + our `EINTR` retry |
| 1.0 | House C++ policy in fork mode; lines K-1/K-2 added meet it (`unique_ptr` instead of naked `delete`, const locker, no pointer arithmetic) | 4ac69d23 | `cpp-policy-check .`, unit 1,954, acceptance 468 | fork-only |
| 1.2 | `FieldConvertors.h` digit check: explicit cast for the deliberate wrap (UBSan `implicit-conversion`) | e9c049cb | rows 1.1, 1.2 under ASan+UBSan | fork-only |
| P2-6 | Upstream unit tests: detach after join, a leaked thread, two leaked objects | 17060e3d | row 1.3, `ut` under TSan | partly upstream 7deb8275 (test half ported) |
| K-3 | `DataDictionary::operator=` kept stale groups, leaked replaced ones, no self-assignment check | 24124fc6 | row 1.4 | fork-only (same pattern as upstream #764 for `FieldMap`) |
| P2-5 | `getField<T>()` / `FIELD_GET_REF` cast the stored `FieldBase` to its type (UB); `FIELD_GET_PTR` removed | 34f7d30f | row 1.5, `ut`/`krx_ut` under UBSan | quickfix/quickfix#735 (open), done differently: 2 definitions instead of every call site |
| K-4 | Generated typed messages cast `FIX::Header` to the version's `Header` (UB); generator and 9 `Message.h` | b18d6c86 | row 1.6 | fork-only (no upstream issue found) |
| 1.4c | Seven `SessionTestCase` sections leaked the fixture's session | 537a9b25 | `ut` under LeakSanitizer | fork-only |
| K-5 | `processQueuedMessages` failed on slow (sanitized) runs: real-time messages vs a fixed session clock | 12cedd84 | row 1.7 | fork-only (test-only) |
| K-6 | Plain `SocketAcceptor` in `poll()` mode leaked its server and port; `SocketServer::close()` let the listening socket be closed up to three times | f86d9387 | row 1.8, acceptance (runs on this class) | related quickfix/quickfix#740 (open PR) |
| P0-1 | A forged data length (`95=`, `RawDataLength` and the like) moved `Message::extractField` past the frame: heap overflow read, found again by our nightly fuzzer | #19 | rows 2.1–2.4, `test/krx/corpus/p0-1-forged-data-length.bin` | port of quickfix/quickfix#779 (open PR); we also reject a negative length |
| P0-2 | A `std::exception` while a message was handled (a SequenceReset moving back before Logon, whose reject throws; an application callback's error) escaped every catch and aborted the whole process | #20 | rows 2.5–2.7 and three edge cases (`[p0-2]`) | ours: `Session::next` (both) and `ThreadedSocketConnection::read` now catch `std::exception`, log it and disconnect that session |
| P0-7 | A garbled frame made every `readMessage` return true without a message, so the previous one was dispatched again (sequence too low, Logout); the parser also cleared every frame queued behind the bad one, and a frame without BodyLength borrowed the next frame's | #21 | rows 2.8–2.10 and three parser edge cases (`[p0-7]`) | ours: `Parser::readNextFixMessage` skips a garbled frame up to the next `\0018=`; BodyLength must follow BeginString; the four connection classes (plain, threaded, both SSL) call it |
| P1-13 | The parser buffered without limit: a frame announcing `9=99999999`, or bytes that never form a frame, grew memory until the process died | #22 | row 2.11 and four parser edge cases (`[p1-13]`) | ours: `Parser::MAX_BODY_LENGTH` (1 MiB) and `MAX_BUFFER_SIZE` (1 MiB + 64 KiB); past either the parser throws `SocketRecvFailed`, which every connection class already logs and disconnects on |
| P1-11 | A ResendRequest range was used unchecked: a negative begin, or a begin after the end, became a store read (with FileStore, one per number in the range) and a SequenceReset from a bogus number | #23 | rows 2.12–2.14 and an exact-range edge (`[p1-11]`) | ours: `nextResendRequest` rejects `BeginSeqNo < 1` or `EndSeqNo < BeginSeqNo` (EndSeqNo 0 = to the end) before the store is read. Note: FIX fields compare by **tag**, so the check compares `getValue()` |
| P2-4 | A stored message without `35=` made the field-order-preserving resend read a MsgType at `npos + 4`; any unreadable stored message stopped the resend part-way | #23 | row 2.15 and two-in-a-row edge (`[p2-4]`) | ours: a stored message without MsgType is gap-filled and logged, like any message not resent |
| P0-3 (3.1) | A send to a counterparty that stopped reading blocked for ever, holding the session lock: no heartbeat, no disconnect, every sender stuck | #25 | row 3.1 and five edges (`[p0-3]`) | ours: the threaded connection's socket is non-blocking from the first send; a full buffer waits up to the session's send timeout (D11: `SendTimeout`, default half of HeartBtInt), then the socket is shut down and the session disconnects. The rest of P0-3 is items 3.2–3.4 |
| P0-3 (3.2) | A peer that vanished (cable, firewall, crashed host) left sent data unacknowledged for the kernel's default ~15 minutes of retransmits before the connection dropped | #26 | row 3.3 (`[p0-3]`) | ours: at the first send the threaded connection's socket gets `SO_KEEPALIVE` and a drop-after of 2 × HeartBtInt (`TCP_USER_TIMEOUT` on Linux, `TCP_RXT_CONNDROPTIME` on macOS). No new setting |
| P1-1 (part: mutex) | `FIX::Mutex`, hand-written recursive over a pthread mutex, read its owner and count without synchronisation (TSan on any two threads sharing a session lock) and a copy copied its lock state | #28 | `TeardownRaceScenarios.cpp` (`[p1-1]`): the copy row hangs before; the two contention rows are red under TSan only | port of quickfix/quickfix#772 (open PR): `std::recursive_mutex`, copies are new unlocked mutexes |
| P1-1 (part: state) | `SessionState`'s logon, logout and enabled flags and its last sent time were read by the reading thread without the session lock while other threads changed them (TSan on a cross-thread `disconnect()` or send) | #29 | `TeardownRaceScenarios.cpp` (`[p1-1]`): concurrent reads and writes (red under TSan only), last sent time round trip to the nanosecond | ports of quickfix/quickfix#773 and #781 (atomic flags) and #774 (the last sent time as one atomic nanosecond count, via new `DateTime::toEpochNanos`/`fromEpochNanos`; without upstream's range checks for before 1970 or after 2262; its test-date fix ported) |
| P0-3 (3.3) | `Session::disconnect()` took the session lock first, so it waited behind a sender stuck in send (up to the send timeout since #25) | #27 | row 3.4 (`[p0-3]`): 9.8 s before, at once after | ours: `disconnect()` first shuts the connection down under a new small `m_responderMutex` (which also guards `setResponder`), then takes the session lock; each responder is still disconnected once. `ThreadedSocketConnection::m_disconnect` is atomic. Needs #28 and #29 for TSan |
| P1-3 | A signal (profiler, debugger, runtime) that interrupted the reading thread's `poll` was taken as a socket error: "Interrupted system call", and the session disconnected | #30 | rows 3.6–3.7 (`[p1-3]`): signal storms on an idle and a busy session | ours: `read()` reads again on `EINTR` from `poll`. `recv` needs nothing: it runs only after `poll` reports data, and an interrupted `recv` that has data returns a short read, not `EINTR` |
| P0-3 (3.4) | A resend held the session lock from the first message to the last, so no sender could place an order until a large resend (thousands of messages, or a slow reader) had finished | this PR | row 3.5 (`[p0-3]`): the order came after all 2,001 resent messages before, during the resend after; a disconnect during a resend stops it (2,000 resent after the disconnect without the check) | ours: every 100 messages `generateRetransmits` releases the session lock until the threads waiting for it in `sendRaw` or `disconnect()` (counted in `m_mutexWaiters`) have taken it, for up to 10 ms, and stops when the connection went away meanwhile. A plain release and yield was not enough on Linux: the resending thread took the lock straight back (first CI run of #31). The store is still read in one go, as upstream does. A sender's order can now go out between resent messages, with a higher sequence number, which FIX allows |

### K-1. A threaded connection's socket was closed twice (10y)

- **Symptom:** at shutdown the gateway aborted with `IO Safety violation: owned file descriptor
  already closed`, after its message log's sync got `EBADF`. Seen once in the dod's
  release-assertions BDD run of step 10w-2 (2026-11-09).
- **Cause:** `ThreadedSocketAcceptor/Initiator::onStop` closed every socket in `m_threads` from
  the stopping thread, while each connection's own thread also closed its socket
  (`ThreadedSocketConnection::disconnect`). The second close hit a number the kernel had
  already given to another file, here the message log.
- **How we found it:** read the close paths, then a test that keeps opening files while ports
  stop and checks each file's inode afterwards. It only failed once the logout callback was
  slow (1.5 s), as the gateway's real callbacks are. A temporary diagnostic in `socket_close`
  (backtrace whenever it was handed something that is not a socket) named
  `ThreadedSocketAcceptor::onStop` and `Session::disconnect` as the two closers.
- **Fix (5d85a415):** a connection's socket is closed only in `~ThreadedSocketConnection`, run
  by its thread in `releaseConnection` under the port's mutex, in the same step that takes it
  out of `m_threads`. `disconnect()` and `onStop` only shut it down. Listening sockets keep
  their single close in `onStop`. Threads that close after the stop began are joined by
  `onStop` (`m_closedThreads`).
- **Checks:** before: 7–22 files lost per 20–60 s (BDD: 5 in 3 rounds). After: 0 in 2 × 60 s.
  `KRX_RACE_SECONDS=60` makes the gateway test run longer.

### K-2. A partial send put bytes from past the message on the wire (#742)

- **Symptom:** none seen in production yet. After a send came back part-way, the rest was sent
  with the whole message's length, so bytes past the end of the message (process memory) went
  to the broker and broke the FIX stream after it.
- **How we found it:** upstream PR quickfix/quickfix#742 (open, unreviewed). Reproduced with a
  broker that reads slowly and a signal that interrupts the blocked send: 2,938–3,770 stray
  bytes after the message.
- **Fix (0efe36b8):** send `msg.length() - totalSent`, as #742 does; also retry a send that a
  signal interrupted before any byte went out (`EINTR`), which used to fail the message.

## Known and not fixed here

- **Plain `SocketAcceptor`: `onStop` can use `m_pServer` after the acceptor's thread released
  it** (a stop from another thread racing the end of `onStart`). The gateway doesn't run this
  class (see above). Related upstream: quickfix/quickfix#740 (open PR).
- **`HttpServer` closes each accepted socket two or three times** (quickfix/quickfix#747, PRs
  #748, #750, #763, #766, none merged). It only runs when a config sets `HttpAcceptPort`; no
  gateway config does (checked 2026-11-09).
- Everything else known is in the plan: P0-1 … P0-11, P1-1 … P1-13 and the rest of P2.

## Other work outside this repo

- **quickfix-rs's `quickfix/tests/test_session.rs` is unreliable** (plan item 9.5). Both tests
  register the same session IDs in QuickFIX's process-wide registry while running in parallel;
  the logon wait `while !sender.is_logged_in() && !receiver.is_logged_in()` stops when either
  side is logged on (`||` was meant); it relies on fixed 50 ms sleeps. Until it is fixed,
  quickfix-rs's `krx_ci.yml` runs the tests one at a time. Run 2026-11-09 on macOS, three times
  each (`cargo test -p quickfix --test test_session`):

  | libquickfix | parallel (default) | one test at a time |
  |---|---|---|
  | upstream `94625580` | SIGSEGV 3 of 3 | pass 3 of 3 |
  | this fork (K-1, K-2) | SIGSEGV 3 of 3 | pass 3 of 3 |

- **Offering our fixes upstream** (K-1, K-2's `EINTR` retry, K-3, K-4, K-6): the owner decides
  first, since the PRs are public (plan Q7).

## Updating from upstream

Take upstream fixes as single cherry-picks with their upstream tests, each with a ledger line.
A rebase onto a newer upstream is its own release with full re-certification (see the traps in
docs/MAINTENANCE.md §7). Drop a ledger line once upstream has merged an equivalent.
