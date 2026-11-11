#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "DataDictionary.h"

namespace krx {

// The dictionaries the fuzz modes parse against: load once, share across inputs.
struct FixDictionaries {
  explicit FixDictionaries(const std::string& spec_dir);

  FIX::DataDictionary fix44;
  FIX::DataDictionary fix42;
  FIX::DataDictionary fixt11;
  FIX::DataDictionary fix50sp2;
};

// One fuzz input through the parser, the message classes and the dictionaries. The first byte picks
// the mode (the research harness's five: FIX.4.4, FIX.4.2, FIXT.1.1 with FIX.5.0SP2, chunked
// Parser, no dictionary); the rest is wire bytes. Returns how many parses the engine rejected with
// a FIX exception, which is expected for bad input; a crash or any other exception is a finding.
[[nodiscard]] std::size_t FeedFixInput(const FixDictionaries& dictionaries, std::string_view input);

// Fuzz seeds from the acceptance definitions: each I8/E8 line made a well-formed frame (fixed
// timestamps, BodyLength and CheckSum recomputed), once per mode that suits it.
[[nodiscard]] std::vector<std::string> SeedsFromDefinitions(const std::string& definitions_dir);

}  // namespace krx

// The libFuzzer entry point (FuzzEntry.cpp, target krx_fuzz).
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);
