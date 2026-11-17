// Test helpers: a temporary directory that deletes itself, and a one-line file writer.
#pragma once

#include <stdlib.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mse {

class TempDir {
 public:
  TempDir() {
    std::string pattern = (std::filesystem::temp_directory_path() / "mse-test-XXXXXX").string();
    if (mkdtemp(pattern.data()) == nullptr) throw std::runtime_error("mkdtemp failed");
    path_ = pattern;
  }
  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

inline void write_file(const std::filesystem::path& path, std::string_view content) {
  std::ofstream out(path, std::ios::binary);
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

}  // namespace mse
