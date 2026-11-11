// libFuzzer entry point (plan items 1.1, 1.7, 8.6): one input through FeedFixInput, the five
// research fuzz modes. Built only with -DKRX_FUZZ=ON and clang. Run, for example:
//   krx_fuzz -max_total_time=1800 corpus/
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "FixInput.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  static const krx::FixDictionaries kDictionaries(KRX_SPEC_DIR);
  const std::span<const std::uint8_t> bytes(data, size);
  static_cast<void>(krx::FeedFixInput(kDictionaries, std::string(bytes.begin(), bytes.end())));
  return 0;
}
