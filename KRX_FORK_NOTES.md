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
| P0-3 (3.4) | A resend held the session lock from the first message to the last, so no sender could place an order until a large resend (thousands of messages, or a slow reader) had finished | #31 | row 3.5 (`[p0-3]`): the order came after all 2,001 resent messages before, during the resend after; a disconnect during a resend stops it (2,000 resent after the disconnect without the check); with no sender waiting, 2,000 messages still resend within 5 s (20 s if it always waited) | ours: before each message `generateRetransmits` releases the session lock until the threads waiting for it in `sendRaw` or `disconnect()` (counted in `m_mutexWaiters`) have taken it, for up to 10 ms, and stops when the connection went away meanwhile. A plain release and yield was not enough on Linux: the resending thread took the lock straight back (first CI run of #31). The store is still read in one go, as upstream does. A sender's order can now go out between resent messages, with a higher sequence number, which FIX allows. Mull: the mutant removing the wait's `yield()` survives and is accepted as equivalent (owner, 12 Nov 2026, MAINTENANCE §5) until plan item 3.8 measures yield against spin |
| P1-4 | Any failed `accept` (a signal, a connection reset before it was accepted, no file descriptor left) | P1-4 | Any failed `accept` (a signal, a connection reset before it was accepted, no file descriptor left) ended the threaded acceptor's listening thread without a log line: the port stopped accepting for good | #32 | rows 3.8–3.9 (`[p1-4]`): 50 signals, then a connection (never accepted before, accepted after); a connection while no descriptor is left, then another (none accepted before; the failure logged and the next accepted after) | ours: the accept loop logs every failure, accepts again at once after `EINTR`/`ECONNABORTED`, after 100 ms after `EMFILE`/`ENFILE`/`ENOBUFS`/`ENOMEM`, and stops on anything else or when the acceptor stops. macOS drops the connection whose accept hit `EMFILE`; Linux keeps it waiting. `ThreadedSSLSocketAcceptor` and the plain `SocketServer` have the same loop and are not changed (the gateway runs neither) |
| P1-10 | The threaded connection ran the session's timers (heartbeat, TestRequest, the heartbeat timeout) only when `poll` waited a whole second for nothing, so a peer sending one byte every half second, or a stream of bytes that never forms a message, was never timed out | #33 | row 3.10 (`[p1-10]`): HeartBtInt 1, then one byte every 500 ms: no TestRequest and no disconnect in 8 s before; "Timed out waiting for heartbeat" and the connection closed after | ours: `read()` runs `Session::next(now)` on any pass when a second has gone by since it last did, whether or not data came |
| P1-12 | The threaded initiator connected with a blocking `connect()`: to a primary host that drops packets it waited for the kernel (75 s on macOS, 127 s on Linux) before it tried `SocketConnectHost1` | #34 | row 3.11 (`[p1-12]`): primary 10.255.255.1 (drops every packet), backup a local acceptor, `ReconnectInterval=1`: still connecting to the primary after 5 s before; the backup accepted within 5 s after | ours: `ThreadedSocketConnection::connect()` connects non-blocking and waits at most 2 s (`poll`, then `SO_ERROR`); a timeout or a signal fails that attempt and the initiator tries the next host. Failover time is then 2 s plus up to `ReconnectInterval`, so the gateway needs a short `ReconnectInterval` for a 5 s failover. Windows keeps the blocking connect |
| row 3.2 | Test only: `stop()` with a sender stuck in send to an exchange that reads nothing | #35 | row 3.2 (`[p0-3]`): `stop()` returns in about 3 s (under the 10 s send timeout) with the sender out of send. Not red: it also passes with 3.3 undone, because `ThreadedSocketAcceptor::onStop` shuts every connection down, which releases the sender. A guard against a `stop()` that hangs | no engine change |
| P0-4 (4.1, 4.2 store) | `FileStore::set` wrote the header record and put the message in its index before the body was written. A failed write (disk full) left a header pointing at half a body, and the stream's sticky error flag then failed every later write and every read, earlier messages included | #36 | rows 4.1–4.2 and a header-write edge (`[p0-4]`, `FaultyFile.h`: `RLIMIT_FSIZE` as a full disk): before, every read failed after the failed write, writes still failed once space returned, and a failed header write left the message readable with a stray body byte; after, earlier messages read back, the failed one is not there, both files are back to their size, and writes work once space returns | ours: body, then header record (now built as a string, same format), each flushed; the index only after both. On any failure `rollBack` closes both files, cuts them back to their size before the write (`truncate`) and reopens them, then throws `IOException`. Bench (file store, medians of 3 interleaved): p99.9 133.3 µs vs 129.1 µs base (+3.3 %), throughput +1.3 %. The session side (fail closed, D7) is the next PR |
| P0-4 (4.2 session, D7) | When the store failed to write an outgoing message, `Session::sendRaw` logged the IO error and returned `false`, and the session stayed logged on and kept taking orders it could not keep | #37 | row 4.3 (`[p0-4]`): an order sent while the store's files may not grow: logged as a plain IO error and still connected before; after, the send fails, "Store write failed, disconnecting: …" is logged and the session disconnects | ours: `sendRaw`'s `IOException` handler (every `IOException` there comes from the store) logs that event and calls `disconnect()`. Inbound store failures already disconnect (P0-2, `ThreadedSocketConnection::read`). A send while not logged on is Phase 5 (P0-6) |
| P0-4 (4.3 restart) | On restart `FileStore::populateCache` loaded every header record, also one whose message runs past the end of the body file (the machine stopped between the two writes, or the body file was cut short). Reading that message threw, and the store reads a resend's range in one call, so the whole resend failed | #38 | row 4.4 (`[p0-4]`): messages 1–3 stored, the body cut short inside 3, the store reopened: 3 gave "Unable to read" and a range read of 1–3 failed before; after, 1 and 2 read back, 3 is not there, the range gives 1 and 2. Edge: a store with one message (its body is the whole body file) reopens with it | ours: `populateCache` skips a record unless `0 <= offset` and `offset + size <= ` the body file's size (no body file: size 0). A dropped message is gap-filled on resend, like any message not resent |
| P2-4 (4.5) | `FileStore::get` built the message with `msg = buffer`, which stops at the first NUL, so a stored message carrying a NUL in a data field (`95`/`96` and the like) came back cut short | #39 | row 4.6 (`[p2-4]`): a 29-byte message with a NUL in `96`: 19 bytes back before, all 29 after | ours: `msg.assign( buffer, size )` |
| P0-5 (4.4, Q1) | The file store flushed to the operating system but never forced data to disk: a host crash or power loss could lose messages and seqnums the session had already sent | #40 | rows 4.5 and two more (`[p0-5]`): with sync on, body, header and seqnums are synced (3 file syncs) before `send()` returns (0 before: the stub); with sync off nothing is synced; `FileStoreSync=Y` in the settings turns it on | ours: new setting `FileStoreSync` (default N, `doc/html/configuration.html`). `set()` marks its message unsynced; the seqnum write that follows syncs body, header and seqnums (`fdatasync`; `fsync` on macOS, `_commit` on Windows): one sync point per message. A failed sync throws, so the session fails closed (#37). `FileStore::getSyncCount()` counts file syncs. Cost (macOS, file store, medians of 3): 66,935 → 12,677 msg/s, RTT p50 59.6 → 200.9 µs, p99.9 118.0 → 372.4 µs; to be measured on the production disk (11.7). The crash runbook (the other half of Q1) is not written yet |
| P0-5 (option B) | #40 synced three files per sent message and one per received message, which cost 81 % of throughput on a Mac (owner: too slow) | this PR | row 4.5 now one file sync per sent message, a received message none, a restart after a crash that lost the header and seqnums writes recovers messages 1–3 and next seqnum 4, a message cut short at the end of the body is removed, only a sender seqnum the body cannot rebuild is synced (`[p0-5]`; all red on #40 first: 3 syncs, 4 for two received, message 3 missing and next 2, 40 stray bytes, the target change synced) | ours: `set()` syncs only the body (a failed sync rolls back like a failed write); `setSeqNum` syncs the seqnums only when the next sender seqnum is past the last stored message; on open `recoverUnindexed` frames the body past the last header record (BodyLength, CheckSum), writes the missing header records, raises the next sender seqnum and cuts an unfinished message. The next target seqnum is not synced: after a crash the exchange resends (PossDup). Mac: 27,124 msg/s (2.1× #40), RTT p50 102.8 µs, p99.9 195.1 µs |

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
