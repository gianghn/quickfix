# V1 feature-files plan

Where each row of [V1_BDD_CHECKLIST.md](V1_BDD_CHECKLIST.md) lives. Library scenarios are Catch2
`SCENARIO`s tagged like Gherkin (`[krx][v1][p0-1]`), so `ut "[krx]"` runs them all and
`ut "[p0-3]"` runs one defect's.

| Scenario file | Tags | Checklist rows | Steps / fixtures | Runner | Phase |
|---|---|---|---|---|---|
| `src/C++/test/krx/HarnessScenarios.cpp` | `[krx][v1][harness]` | 1.1–1.3 | `src/C++/test/krx/SessionPair.h` | Catch2 `ut` | 1 |
| `src/C++/test/krx/InboundFramingScenarios.cpp` | `[krx][v1][security]` + defect tag | 2.1–2.15 | `SessionPair.h`, `test/krx/corpus/` | Catch2 `ut` | 2 |
| `src/C++/test/krx/SocketLivenessScenarios.cpp` | `[krx][v1][security]` + defect tag | 3.1–3.11 | `SessionPair.h`, `StalledPeer.h` | Catch2 `ut` | 3 |
| `src/C++/test/krx/StoreDurabilityScenarios.cpp` | `[krx][v1]` + defect tag | 4.1–4.6 | `FaultyFile.h` (RLIMIT_FSIZE, read-only dir) | Catch2 `ut` | 4 |
| `src/C++/test/krx/SendTruthScenarios.cpp` | `[krx][v1]` + defect tag | 5.1–5.5 | `SessionPair.h` | Catch2 `ut` | 5 |
| `src/C++/test/krx/TeardownRaceScenarios.cpp` | `[krx][v1][tsan]` + defect tag | 6.1–6.6 | `SessionPair.h` | Catch2 `ut` (TSan job) | 6 |
| `src/C++/test/krx/UpstreamPicksScenarios.cpp` | `[krx][v1][upstream]` | 7.1–7.6 | upstream tests as picked | Catch2 `ut` | 7 |
| `quickfix/tests/test_callback_panic.rs` (quickfix-rs) | — | 9.1–9.3 | — | `cargo test` | 9 |
| `tests/features/order_safety.feature` (gateway) | `@krx @v1 @order` | 10.1–10.6 | gateway steps | cucumber-rs | 10 |
| `tests/features/callback_discipline.feature` (gateway) | `@krx @v1 @concurrency` | 10.7 | gateway steps | cucumber-rs | 10 |
| `tests/features/session_conformance.feature` (gateway) | `@krx @v1 @conformance` | 10.8–10.16 | simulator steps | cucumber-rs | 10 |
| `tests/features/krx_schedule.feature` (gateway) | `@krx @v1 @schedule` | 10.17–10.19 | simulator steps | cucumber-rs | 10 |
| `tests/features/pre_trade_controls.feature` (gateway) | `@krx @v1 @risk` | 10.20–10.24 | gateway steps | cucumber-rs | 10 |
| `tests/features/faults/network_faults.feature` (gateway) | `@krx @v1 @fault` | 11.1–11.2 | toxiproxy hooks | cucumber-rs | 11 |
| `tests/features/faults/process_disk_clock.feature` (gateway) | `@krx @v1 @fault` | 11.3–11.8 | lab hooks | cucumber-rs (lab) | 11 |

Long runs (U-4…U-8) are scripts in `test/krx/` (`run_stress.sh`, fuzz drivers), not scenarios;
their logs go in the phase handover.
