# V1 BDD checklist — KRX fork go-live

One row per rule of [V1_PLAN.md](V1_PLAN.md). Tick a row only when its scenario passes in Release,
ASan+UBSan and TSan. Every Phase 2–7 row must fail on `25273cc3` first; its red output goes in the
phase handover.

Who is who: **the gateway** is DNSE's KRX gateway on this engine; **the exchange** is HOSE/KRX (a
simulator in tests); **a sender** is a gateway thread placing orders; **an operator** watches the
alerts.

Runners (plan D2): library rows are Catch2 `SCENARIO`s in `src/C++/test/krx/`, run by `ut`;
quickfix-rs rows are `cargo test`; gateway rows are Gherkin in
`krx_gateway_rust/tests/features/`. The file map is [V1_FEATURES_PLAN.md](V1_FEATURES_PLAN.md).

## 1. Harness — Phase 1 (src/C++/test/krx/HarnessScenarios.cpp)

| # | Scenario | Plan |
|---|---|---|
| [x] 1.1 | A session can be driven byte by byte over a socketpair without a network | 1.2 |
| [x] 1.2 | Every acceptance-test message replays through the parser and the dictionaries | 1.3 |
| [x] 1.3 | The whole unit suite runs to the end under TSan and LSan | 1.4, 1.4c |
| [x] 1.4 | Assigning one data dictionary over another frees what it held | 1.4a |
| [x] 1.5 | Reading a field from a message never treats it as a type it is not | 1.4b |
| [x] 1.6 | A typed message's header is never treated as a type it is not | 1.4d |
| [x] 1.7 | A new gap after a long queue drains is requested again (also on a slow, sanitized run) | 1.4e |
| [x] 1.8 | A polled plain acceptor gives its port back when destroyed, and closes its listening socket once | 1.4f |

## 2. Hostile input — Phase 2 (src/C++/test/krx/InboundFramingScenarios.cpp)

| # | Scenario | Plan |
|---|---|---|
| [x] 2.1 | A frame whose data length points past the end of the buffer is rejected as invalid, and the process keeps running | 2.1 |
| [x] 2.2 | A negative data length is rejected as invalid | 2.1 |
| [x] 2.3 | A data length of two billion is rejected as invalid | 2.1 |
| [x] 2.4 | The fuzzer's recorded crash input no longer crashes the parser | 2.1 |
| [x] 2.5 | A SequenceReset that arrives before Logon ends the connection, and the process keeps running | 2.2 |
| [x] 2.6 | An unexpected error raised in the gateway's `fromApp` disconnects the session and is logged, and the process keeps running | 2.3 |
| [x] 2.7 | An unexpected error in a connection thread is logged and disconnects only that session | 2.3 |
| [x] 2.8 | After a garbled frame, the next good message is processed and the session stays logged on | 2.4 |
| [x] 2.9 | A garbled frame is never answered by re-delivering the previous message | 2.4 |
| [x] 2.10 | Good messages queued behind a garbled frame are still delivered | 2.4 |
| [x] 2.11 | A frame announcing a body larger than the cap disconnects the session with bounded memory | 2.5 |
| [x] 2.12 | A ResendRequest with a negative begin is rejected without a store lookup | 2.6 |
| [x] 2.13 | A ResendRequest whose begin is after its end is rejected | 2.6 |
| [x] 2.14 | A ResendRequest with end 0 resends everything from begin | 2.6 |
| [x] 2.15 | A resend with field order preserved survives a stored message missing its MsgType (gap-filled) | 2.7 |

## 3. Socket liveness — Phase 3 (src/C++/test/krx/SocketLivenessScenarios.cpp)

| # | Scenario | Plan |
|---|---|---|
| [x] 3.1 | When the exchange stops reading, senders fail within the send timeout and the session disconnects | 3.1 |
| [x] 3.2 | When the exchange stops reading, `stop()` returns and no thread is left in send | 3.1, 3.3 |
| [x] 3.3 | Every session socket has keepalive and a user timeout of about two heartbeats | 3.2 |
| [x] 3.4 | `disconnect()` releases a sender stuck in send before it waits for the session lock | 3.3 |
| [x] 3.5 | A sender can place an order while a large resend is in progress | 3.4 |
| [x] 3.6 | A signal storm on an idle session causes no disconnect | 3.5 |
| [x] 3.7 | A signal storm on a busy session causes no disconnect | 3.5 |
| [x] 3.8 | The acceptor keeps taking connections after an interrupted or aborted accept | 3.6 |
| [x] 3.9 | The acceptor backs off and recovers when it runs out of file descriptors | 3.6 |
| [x] 3.10 | A peer trickling one byte every 500 ms is timed out per HeartBtInt | 3.7 |
| [x] 3.11 | When the primary exchange host is unreachable, the initiator reaches the backup host within 5 s | 3.7 |
| [ ] 3.12 | Latency percentiles: p50, p99, p99.9 and max of known samples are exact; one sample is every percentile; no samples is reported as none | 3.8a |
| [ ] 3.13 | A small resend benchmark (500 messages, one sender) reports every message resent, at least one order placed during the resend, and a time for each order | 3.8b |
| [ ] 3.14 | A resend benchmark with no sender reports the resend alone and no orders, so runs with and without contention compare | 3.8b |

## 4. Store durability — Phase 4 (src/C++/test/krx/StoreDurabilityScenarios.cpp)

| # | Scenario | Plan |
|---|---|---|
| [x] 4.1 | After a failed write, earlier messages can still be read from the store | 4.1, 4.2 |
| [x] 4.2 | After a failed write, the store accepts writes once space returns | 4.2 |
| [x] 4.3 | A failed write stops order flow and disconnects the session with an error logged | 4.2 |
| [x] 4.4 | A restart after a failed write loads only consistent records | 4.3 |
| [ ] 4.5 | With store sync on, a stored message and its seqnum are on disk before `send()` returns | 4.4 |
| [x] 4.6 | A message with an embedded NUL comes back from the store whole | 4.5 |

## 5. No silent loss — Phase 5 (src/C++/test/krx/SendTruthScenarios.cpp)

| # | Scenario | Plan |
|---|---|---|
| [ ] 5.1 | An order sent before Logon reports not sent | 5.1 |
| [ ] 5.2 | An order whose socket write fails reports not sent | 5.1 |
| [ ] 5.3 | A store failure while persisting an order leaves no stored copy that a retry would duplicate | 5.2 |
| [ ] 5.4 | Every outbound Reject or BusinessMessageReject is logged at error level with its seqnum and reason | 5.3 |
| [ ] 5.5 | An execution report with an unknown tag reaches `fromApp` flagged as failed validation (only if Q3 picks the engine patch) | 5.4 |

## 6. Teardown and locking — Phase 6 (src/C++/test/krx/TeardownRaceScenarios.cpp)

| # | Scenario | Plan |
|---|---|---|
| [ ] 6.1 | Many senders while the connection drops raise no TSan report and no use-after-free | 6.1, 6.2 |
| [ ] 6.2 | A dropped connection always clears the session's logon state | 6.1 |
| [ ] 6.3 | The threaded acceptance suite raises no unsuppressed TSan report | 6.3 |
| [ ] 6.4 | `onLogout` runs after the session lock is released | 6.4 |
| [ ] 6.5 | Repeated forced and graceful acceptor stops never hang and never close a foreign file | 6.5 |
| [ ] 6.6 | A connection that fails to attach to its session leaves the session free for the next one | 6.6 |

## 7. Upstream picks — Phase 7 (src/C++/test/krx/UpstreamPicksScenarios.cpp, plus upstream tests)

| # | Scenario | Plan |
|---|---|---|
| [ ] 7.1 | A GapFill leaves no skipped message waiting in the queue | 7.1 |
| [ ] 7.2 | A field map moved onto itself or onto a map with groups keeps its contents without leaking | 7.2 |
| [ ] 7.3 | A NonStopSession keeps its seqnums across a restart (only if Q5 = yes) | 7.3 |
| [ ] 7.4 | A TLS session survives certificate rotation and more than 1,024 open files (only if built-in TLS) | 7.4 |
| [ ] 7.5 | Nested repeating groups with correct counts pass validation | 7.6 |
| [ ] 7.6 | Session settings for validation also apply to the default dictionary | 7.6 |

## 9. quickfix-rs — Phase 9 (gianghn/quickfix-rs: quickfix/tests/test_callback_panic.rs)

| # | Scenario | Plan |
|---|---|---|
| [ ] 9.1 | A panic in a callback is never reported to the engine as success | 9.1 |
| [ ] 9.2 | A panic in `from_app` does not advance the expected seqnum, and the message comes back on reconnect | 9.2 |
| [ ] 9.3 | Two session tests can run in parallel without crashing | 9.5 |

## 10. Gateway — Phase 10 (krx_gateway_rust/tests/features/)

| # | Scenario | Plan |
|---|---|---|
| [ ] 10.1 | The gateway refuses an order while the session is not logged on and tells the OMS | 10.1 |
| [ ] 10.2 | A resent order older than the staleness limit is gap-filled and reported to the OMS as not sent | 10.1 |
| [ ] 10.3 | An order is journalled by ClOrdID before it reaches the exchange | 10.2 |
| [ ] 10.4 | An order resent after a crash is not placed twice | 10.2 |
| [ ] 10.5 | An execution report with an extra KRX tag reaches the OMS | 10.3 |
| [ ] 10.6 | An operator is alerted on every outbound Reject | 10.3 |
| [ ] 10.7 | A callback that sends an order while another thread holds the order lock does not deadlock | 10.4 |
| [ ] 10.8 | The 59 FIX session test cases pass against the exchange simulator | 10.5 |
| [ ] 10.9 | After a reconnect with gaps in both directions, every order and fill is accounted for | 10.6 |
| [ ] 10.10 | After kill -9 and a restart from the store, no order is lost or duplicated | 10.6 |
| [ ] 10.11 | The start-of-day reset follows the HOSE schedule | 10.6 |
| [ ] 10.12 | A logon outside the trading window is refused cleanly | 10.6 |
| [ ] 10.13 | A resend of thousands of messages at 09:00 does not stop new orders | 10.6 |
| [ ] 10.14 | A SequenceReset-Reset to a lower number is handled per the spec | 10.6 |
| [ ] 10.15 | A garbled frame mid-burst loses no order | 10.6 |
| [ ] 10.16 | An exchange disconnect mid-burst loses no order | 10.6 |
| [ ] 10.17 | The session stays up through the lunch break | 10.7 |
| [ ] 10.18 | An amendment in a periodic call auction is accepted only in its first 10 minutes | 10.7 |
| [ ] 10.19 | Orders are accepted again after a halt resumes | 10.7 |
| [ ] 10.20 | An order outside the price band is stopped before it reaches the exchange | 10.8 |
| [ ] 10.21 | An order over the quantity or value limit is stopped before it reaches the exchange | 10.8 |
| [ ] 10.22 | The gateway throttles before the exchange limit is reached | 10.8 |
| [ ] 10.23 | A duplicate order is stopped | 10.8 |
| [ ] 10.24 | The kill switch cancels all open orders and blocks new ones within the target time | 10.8 |

## 11. Faults — Phase 11 (krx_gateway_rust/tests/features/faults/)

| # | Scenario | Plan |
|---|---|---|
| [ ] 11.1 | Under delay, loss, reorder and duplication the session recovers with no lost or duplicate order | 11.1 |
| [ ] 11.2 | When the exchange stops reading, the gateway disconnects within the heartbeat bound | 11.1 |
| [ ] 11.3 | SIGKILL at random points in a burst loses and duplicates no order (100 runs) | 11.2 |
| [ ] 11.4 | A signal storm causes no disconnect | 11.2 |
| [ ] 11.5 | A full or failing store disk stops order flow with an alert within 5 s, and recovers without manual repair | 11.3 |
| [ ] 11.6 | A clock step of ±30 s or ±5 min causes no logout and no seqnum reset | 11.4 |
| [ ] 11.7 | A power-off during a burst ends, after restart, in a state that matches the exchange | 11.5 |
| [ ] 11.8 | Running out of files, memory or CPU degrades the gateway with alerts, never silently | 11.6 |

## Outside the suite

| # | Check | Plan |
|---|---|---|
| [ ] U-1 | Mull score ≥ 80 % on every changed critical function; survivors killed or recorded as equivalent | 1.7, all fix phases |
| [ ] U-2 | Baseline recorded: red list, bench p50/p99/p99.9, Mull scores | 1.8 |
| [ ] U-15 | CI's sanitize job is green: ASan+UBSan+LSan and TSan over `ut`, `krx_ut`, acceptance (+ threaded under ASan) | 1.5 |
| [ ] U-3 | `cpp-policy-check .` (house policy, fork mode) passes before every PR, its result in the PR | 1.6 |
| [ ] U-4 | P0-10 root cause written down; 50+ seeded chaos runs end with LOST = 0 and every gap explained | 8.1–8.3 |
| [ ] U-5 | TSan stress ≥ 1 h on each of 10 seeds: zero unsuppressed reports | 8.4 |
| [ ] U-6 | Echo-on chaos 20 runs with no hang; Release soak ≥ 8 h with LOST = DUP = OOO = 0 and flat RSS and fds | 8.5 |
| [ ] U-7 | libFuzzer ≥ 24 CPU-h per mode and stateful session fuzzing 24 CPU-h, no crash | 8.6 |
| [ ] U-8 | Shutdown under load: 100 iterations, each exits within 15 s | 8.7 |
| [ ] U-9 | FileStore sync cost measured (p50/p99.9, on and off) | 4.4 |
| [ ] U-10 | `cargo-mutants` on the quickfix-rs callback module: no surviving mutant on the panic path | 9.5 |
| [ ] U-11 | Performance baseline on production-like hardware, 2× peak full-day soak, ATO/ATC burst replay | 11.7, 11.8 |
| [ ] U-12 | Nightly p99.9 regression job ≤ 10 % | 11.8 |
| [ ] U-13 | checksec passes on the release build; no p99 regression from hardening flags | 12.2 |
| [ ] U-14 | Build reproducible from signed tag `krx-1.0.0`; SBOM with both SHAs; no open high or critical CVE | 12.3, 12.4 |

## Security

| # | Check | Plan |
|---|---|---|
| [ ] S-1 | No inbound byte sequence can crash the process (rows 2.1–2.11, U-7) | 2.1–2.5, 8.6 |
| [ ] S-2 | A slow or silent peer cannot freeze other sessions or senders (rows 3.1–3.4, 3.10) | 3.1–3.3, 3.7 |
| [ ] S-3 | A forged ResendRequest cannot tie up the session lock (rows 2.12–2.14, 3.5) | 2.6, 3.4 |
| [ ] S-4 | Store files and logs are 0600; tags 554 and 925 are masked in shipped logs | 13.6 |
| [ ] S-5 | Acceptor ports have an allow-list, a connection cap and a pre-logon timeout; `HttpAcceptPort` unset | 13.6 |
| [ ] S-6 | Encryption decision (Q6) recorded; certificate expiry and rotation tested in UAT | 13.6 |
| [ ] S-7 | Pen test and vulnerability scan filed with no open high finding | 13.6 |

## Go-live evidence — Phases 13–14 (not scenarios; each needs a named owner and filed evidence)

| # | Gate | Plan |
|---|---|---|
| [ ] G-1 | Dashboards live and every alert fired in a drill | 13.1, 13.2 |
| [ ] G-2 | Runbooks rehearsed; reconciliation after every reconnect and at EOD shows no unexplained break | 13.3 |
| [ ] G-3 | Failover, split brain, backup host and DR drills pass within RTO | 13.4 |
| [ ] G-4 | Store volume alerts, Reset policy sign-off, 10-year retention with tested restore | 13.5 |
| [ ] G-5 | SSC 379 and VNX 01 compliance sign-off; RTS 6 / 15c3-5 mapping approved | 13.7 |
| [ ] G-6 | Signed risk record for every P1 item not fixed | 13.8 |
| [ ] G-7 | HOSE/KRX certification passed on `krx-1.0.0`; UAT replay with zero outbound 35=3 | 14.1, 14.2 |
| [ ] G-8 | Go/no-go minutes signed by the named approvers | 14.3 |
| [ ] G-9 | Rollout phases with exit criteria; rollback drill within target | 14.4, 14.5 |
| [ ] G-10 | Hypercare roster published; day-1 EOD reconciliation with zero unexplained breaks | 14.6 |
