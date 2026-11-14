#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace krx {

// One stateful fuzz input (plan item 8.6) through a live acceptor session on a socketpair. The
// first byte picks the state the session is brought to first (before logon, mid-resend: it asked
// for a gap and queues what follows, during logout: it sent its Logout and waits for the answer);
// the rest is the counterparty's bytes, written in chunks, then the session's timers run once and
// the counterparty hangs up. The reading runs on the caller's thread, so a run is deterministic.
// True when the session was in the input's state before the bytes arrived; a crash, a hang or an
// escaping exception is a finding.
[[nodiscard]] bool FeedSessionInput(std::string_view input);

// Fuzz seeds for FeedSessionInput: for each state, a few message sequences that suit it, with a
// fixed SendingTime (the fuzz session does not check latency).
[[nodiscard]] std::vector<std::string> SessionSeeds();

}  // namespace krx
