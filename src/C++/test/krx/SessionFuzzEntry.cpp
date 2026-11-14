// libFuzzer entry point for stateful session fuzzing (plan item 8.6): one input through
// FeedSessionInput, a live session before logon, mid-resend or during logout. Built only with
// -DKRX_FUZZ=ON and clang. The seeds are written into the first corpus directory at start, so a
// run needs no seed files. Run, for example:
//   krx_session_fuzz -max_total_time=1800 -max_len=4096 corpus/
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "SessionInput.h"

extern "C" int LLVMFuzzerInitialize(int* argc, char*** argv);
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

extern "C" int LLVMFuzzerInitialize(int* argc, char*** argv) {
  const std::span<char*> args(*argv, static_cast<std::size_t>(*argc));
  for (const std::string_view arg : args.subspan(1)) {
    if (arg.starts_with('-') || !std::filesystem::is_directory(arg)) {
      continue;
    }
    const std::vector<std::string> seeds = krx::SessionSeeds();
    for (std::size_t i = 0; i < seeds.size(); ++i) {
      std::ofstream(std::filesystem::path(arg) / ("seed-" + std::to_string(i)), std::ios::binary)
          << seeds[i];
    }
    break;
  }
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::span<const std::uint8_t> bytes(data, size);
  static_cast<void>(krx::FeedSessionInput(std::string(bytes.begin(), bytes.end())));
  return 0;
}
