#pragma once

#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <string>

namespace krx {

// While it lives, no file of this process may grow past `bytes` (RLIMIT_FSIZE): a write past it
// fails with EFBIG, as on a full disk. SIGXFSZ, which would end the process, is ignored meanwhile.
class FileSizeLimit {
 public:
  explicit FileSizeLimit(std::uintmax_t bytes) : saved_signal_(std::signal(SIGXFSZ, SIG_IGN)) {
    getrlimit(RLIMIT_FSIZE, &saved_);
    rlimit limited = saved_;
    limited.rlim_cur = std::min<rlim_t>(saved_.rlim_cur, static_cast<rlim_t>(bytes));
    setrlimit(RLIMIT_FSIZE, &limited);
  }

  ~FileSizeLimit() {
    setrlimit(RLIMIT_FSIZE, &saved_);
    std::signal(SIGXFSZ, saved_signal_);
  }

  FileSizeLimit(const FileSizeLimit&) = delete;
  FileSizeLimit& operator=(const FileSizeLimit&) = delete;
  FileSizeLimit(FileSizeLimit&&) = delete;
  FileSizeLimit& operator=(FileSizeLimit&&) = delete;

 private:
  rlimit saved_{};
  void (*saved_signal_)(int);
};

// A fresh, empty directory of its own under the system temp directory, removed when it goes.
class TempDir {
 public:
  explicit TempDir(const std::string& name)
      : path_(std::filesystem::temp_directory_path() /
              ("krx-" + name + "-" + std::to_string(getpid()))) {
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }

  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  TempDir(TempDir&&) = delete;
  TempDir& operator=(TempDir&&) = delete;

  [[nodiscard]] const std::filesystem::path& Path() const { return path_; }

 private:
  std::filesystem::path path_;
};

}  // namespace krx
