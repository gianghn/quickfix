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

- **Plain `SocketAcceptor` closes its listening socket up to three times** (`onStop`, then
  `onStart`'s end, then `~SocketMonitor`), and `onStop` can use `m_pServer` after the
  acceptor's thread deleted it. The gateway doesn't run this class (see above). Related
  upstream: quickfix/quickfix#740 (open).
- **`HttpServer` closes each accepted socket two or three times** (quickfix/quickfix#747, PRs
  #748, #750, #763, #766, none merged). It only runs when a config sets `HttpAcceptPort`; no
  gateway config does (checked 2026-11-09).

## Updating from upstream

Rebase this branch on the new upstream commit, re-run the gateway's checks above and its whole
definition of done, then point the quickfix-rs submodule at the new commit. Drop a fix here
once upstream has merged an equivalent.
