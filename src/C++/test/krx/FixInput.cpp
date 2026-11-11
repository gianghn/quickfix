#include "FixInput.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <numeric>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "DataDictionary.h"
#include "Exceptions.h"
#include "Message.h"
#include "Parser.h"

namespace krx {
namespace {

constexpr char kSoh = '\x01';
constexpr std::size_t kModes = 5;
constexpr std::size_t kChunkSpread = 17;

enum class Mode : unsigned char { kFix44, kFix42, kFixt11, kChunked, kNoDictionary };

// A frame through setString, the message's own output and full validation.
void ParseWith(const std::string& frame, const FIX::DataDictionary& dictionary) {
  FIX::Message message;
  message.setString(frame, true, &dictionary);
  std::string out;
  message.toString(out);
  static_cast<void>(message.toXML());
  FIX::DataDictionary::validate(message, &dictionary, &dictionary);
}

void ParseTransport(const std::string& frame, const FixDictionaries& dictionaries) {
  const FIX::Message message(frame, dictionaries.fixt11, dictionaries.fix50sp2, true);
  std::string out;
  message.toString(out);
  FIX::DataDictionary::validate(message, &dictionaries.fixt11, &dictionaries.fix50sp2);
}

// The bytes arrive in chunks of 1 to 17, as from a socket; every frame the parser yields is parsed.
void ParseChunked(const std::string& bytes, const FIX::DataDictionary& dictionary) {
  FIX::Parser parser;
  const std::size_t chunk = 1 + (bytes.size() % kChunkSpread);
  std::string frame;
  for (std::size_t pos = 0; pos < bytes.size(); pos += chunk) {
    const std::string piece = bytes.substr(pos, chunk);
    parser.addToStream(piece);
    while (parser.readFixMessage(frame)) {
      ParseWith(frame, dictionary);
    }
  }
}

void ParseBare(const std::string& frame) {
  const FIX::Message message(frame, true);
  std::string out;
  message.toString(out);
}

void Feed(const FixDictionaries& dictionaries, Mode mode, const std::string& bytes) {
  switch (mode) {
    case Mode::kFix44:
      ParseWith(bytes, dictionaries.fix44);
      return;
    case Mode::kFix42:
      ParseWith(bytes, dictionaries.fix42);
      return;
    case Mode::kFixt11:
      ParseTransport(bytes, dictionaries);
      return;
    case Mode::kChunked:
      ParseChunked(bytes, dictionaries.fix44);
      return;
    case Mode::kNoDictionary:
      ParseBare(bytes);
      return;
  }
}

std::string WithFixedTimes(std::string line) {
  constexpr std::string_view kTime = "<TIME>";
  for (std::size_t at = line.find(kTime); at != std::string::npos; at = line.find(kTime, at)) {
    line.replace(at, kTime.size(), "20261110-10:00:00.000");
  }
  for (std::size_t at = line.find('<'); at != std::string::npos; at = line.find('<', at)) {
    const std::size_t end = line.find('>', at);
    if (end == std::string::npos) {
      break;
    }
    line.replace(at, end - at + 1, "20261110-10:00:00");
  }
  return line;
}

std::vector<std::string> Fields(const std::string& line) {
  std::vector<std::string> fields;
  std::size_t start = 0;
  while (start < line.size()) {
    const std::size_t end = std::min(line.find(kSoh, start), line.size());
    if (end > start) {
      fields.push_back(line.substr(start, end - start));
    }
    start = end + 1;
  }
  return fields;
}

// BeginString, a BodyLength that matches, the other fields, then the CheckSum.
std::string Framed(const std::string& line) {
  std::vector<std::string> fields = Fields(line);
  std::erase_if(fields, [](const std::string& field) {
    return field.starts_with("9=") || field.starts_with("10=");
  });
  if (fields.empty()) {
    return {};
  }
  std::string body;
  for (std::size_t i = 1; i < fields.size(); ++i) {
    body += fields[i] + kSoh;
  }
  std::string frame = std::format("{}{}9={}{}{}", fields.front(), kSoh, body.size(), kSoh, body);
  const unsigned sum = std::accumulate(frame.begin(), frame.end(), 0U, [](unsigned total, char c) {
    return total + static_cast<unsigned char>(c);
  });
  return frame + std::format("10={:03}{}", sum % 256, kSoh);
}

Mode ModeFor(const std::string& frame) {
  const std::string begin = frame.substr(2, 7);
  if (begin == "FIX.4.4") {
    return Mode::kFix44;
  }
  if (begin == "FIX.4.2") {
    return Mode::kFix42;
  }
  if (begin == "FIXT.1.") {
    return Mode::kFixt11;
  }
  return Mode::kNoDictionary;
}

std::string Seed(Mode mode, const std::string& frame) {
  return static_cast<char>(mode) + frame;
}

}  // namespace

FixDictionaries::FixDictionaries(const std::string& spec_dir)
    : fix44(spec_dir + "/FIX44.xml"),
      fix42(spec_dir + "/FIX42.xml"),
      fixt11(spec_dir + "/FIXT11.xml"),
      fix50sp2(spec_dir + "/FIX50SP2.xml") {}

std::size_t FeedFixInput(const FixDictionaries& dictionaries, std::string_view input) {
  if (input.empty()) {
    return 0;
  }
  const auto mode = static_cast<Mode>(static_cast<unsigned char>(input.front()) % kModes);
  try {
    Feed(dictionaries, mode, std::string(input.substr(1)));
  } catch (const FIX::Exception&) {
    return 1;
  }
  return 0;
}

std::vector<std::string> SeedsFromDefinitions(const std::string& definitions_dir) {
  std::set<std::string> seeds;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(definitions_dir)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".def") {
      continue;
    }
    std::ifstream file(entry.path(), std::ios::binary);
    std::string line;
    while (std::getline(file, line)) {
      if (!line.starts_with("I8") && !line.starts_with("E8")) {
        continue;
      }
      const std::string frame = Framed(WithFixedTimes(line.substr(1)));
      if (frame.empty()) {
        continue;
      }
      for (const Mode mode : {ModeFor(frame), Mode::kChunked, Mode::kNoDictionary}) {
        seeds.insert(Seed(mode, frame));
      }
    }
  }
  return {seeds.begin(), seeds.end()};
}

}  // namespace krx
