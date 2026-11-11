# KRX fork notes

This fork (`gianghn/quickfix`, branch `krx/fd-double-close`) carries fixes the KRX gateway
(`enCapital/krx_gateway_rust`) needs and upstream `quickfix/quickfix` has not merged. It is
based on upstream commit `94625580` (2024-07-24), the one quickfix-rs vendored. Keep this file
up to date when a fix is added, sent upstream or merged there.

## Which classes the gateway runs

The gateway asks quickfix-rs for `FixSocketServerKind::SingleThreaded`. Our quickfix-rs passes
`is_single_threaded()` into the C++ flag `isMultiThreaded`, so that builds
**`ThreadedSocketAcceptor` / `ThreadedSocketInitiator`**. Upstream quickfix-rs fixed the swap
(arthurlm/quickfix-rs#36, 2026-10-04). Taking that fix would move the gateway to the plain
`SocketAcceptor`, which has its own triple close (below), so don't take it without fixing that
first and re-running the checks.

## Fixes in this fork

### 1. A threaded connection's socket was closed twice (10y)

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
- **Checks:** `tests/test_fd_double_close.rs` and the first rule of
  `tests/features/fix_connection_safety.feature`. Before: 7–22 files lost per 20–60 s (BDD: 5
  in 3 rounds). After: 0 in 2 × 60 s. `KRX_RACE_SECONDS=60` makes the test run longer.
- **Upstream:** no issue or PR (checked 2026-11-09); master still has it.

### 2. A partial send put bytes from past the message on the wire (#742)

- **Symptom:** none seen in production yet. After a send came back part-way, the rest was sent
  with the whole message's length, so bytes past the end of the message (process memory) went
  to the broker and broke the FIX stream after it.
- **How we found it:** upstream PR quickfix/quickfix#742 (open, unreviewed). Reproduced with a
  broker that reads slowly and a signal that interrupts the blocked send: 2,938–3,770 stray
  bytes after the message.
- **Fix (0efe36b8):** send `msg.length() - totalSent`, as #742 does; also retry a send that a
  signal interrupted before any byte went out (`EINTR`), which used to fail the message.
- **Checks:** `tests/test_partial_send.rs` and the second rule of
  `tests/features/fix_connection_safety.feature`.

## Known and not fixed here

- **Plain `SocketAcceptor`: `onStop` can use `m_pServer` after the acceptor's thread released
  it** (a stop from another thread racing the end of `onStart`). The triple close of its
  listening socket and the server leaked in `poll()` mode are fixed (K-6, docs/V1_PLAN.md 1.4f).
  The gateway doesn't run this class (see above). Related upstream: quickfix/quickfix#740 (open).
- **`HttpServer` closes each accepted socket two or three times** (quickfix/quickfix#747, PRs
  #748, #750, #763, #766, none merged). It only runs when a config sets `HttpAcceptPort`; no
  gateway config does (checked 2026-11-09).

## Next work (for a separate agent, agreed with the owner 2026-11-09)

Both forks are in the gateway's build: this one (`gianghn/quickfix`, branch `krx/fd-double-close`)
and `gianghn/quickfix-rs` (branch `thread-mod`), vendored at `vendor/quickfix-rs` in
`enCapital/krx_gateway_rust`. Each fork has a `krx_ci.yml` workflow for its branch.

Checked so far on this branch: QuickFIX's unit tests (53 cases, 1,954 assertions) and acceptance
tests (468) pass on macOS; the gateway's whole definition of done passes with both fixes.

1. **Make quickfix-rs's `quickfix/tests/test_session.rs` reliable.** It failed locally on this
   branch: once a segfault after `test_session_set_next_sender_target_msg_seq_num` hung, once
   `test_session_login_logout` at line 70. Its own faults: both tests register the same session
   IDs in QuickFIX's process-wide registry while running in parallel; the logon wait
   `while !sender.is_logged_in() && !receiver.is_logged_in()` stops when either side is logged
   on (`||` was meant); it relies on fixed 50 ms sleeps. Whether it also fails on upstream
   `94625580`: see the result recorded below. Fix the test, then find out whether a real defect
   remains (it uses the same `ThreadedSocket*` classes as the gateway).
2. **Get both forks' CI green** on Linux and macOS.
3. **Fix the plain `SocketAcceptor` triple close** (above), test first, before quickfix-rs's
   corrected server-kind mapping (arthurlm/quickfix-rs#36) is ever taken.
4. **Offering the fixes upstream** (our double close; #742 plus the `EINTR` retry): the owner
   decides first, since the PRs are public.

Ways of working: tests before code (a red run recorded, then green), every vendor change approved
by the owner, push only to the forks, and update this file with each change.

### `test_session` on upstream `94625580` versus this branch

Run 2026-11-09 on macOS, three times each (`cargo test -p quickfix --test test_session`):

| libquickfix | parallel (default) | one test at a time |
|---|---|---|
| upstream `94625580` | SIGSEGV 3 of 3 | pass 3 of 3 |
| this branch | SIGSEGV 3 of 3 | pass 3 of 3 |

The crash predates our fixes: it comes from the two tests sharing the session registry. Until
item 1 is done, quickfix-rs's `krx_ci.yml` runs the tests one at a time.

## Updating from upstream

Rebase this branch on the new upstream commit, re-run the gateway's checks above and its whole
definition of done, then point the quickfix-rs submodule at the new commit. Drop a fix here
once upstream has merged an equivalent.
