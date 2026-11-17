// A list of files that are deleted when the list goes away, unless it was moved from.
#pragma once

#include <filesystem>
#include <utility>
#include <vector>

namespace mse {

class TempFiles {
 public:
  TempFiles() = default;
  ~TempFiles() { remove_all(); }
  TempFiles(const TempFiles&) = delete;
  TempFiles& operator=(const TempFiles&) = delete;
  TempFiles(TempFiles&& other) noexcept : paths_(std::exchange(other.paths_, {})) {}
  TempFiles& operator=(TempFiles&& other) noexcept {
    if (this != &other) {
      remove_all();
      paths_ = std::exchange(other.paths_, {});
    }
    return *this;
  }

  void add(std::filesystem::path path) { paths_.push_back(std::move(path)); }

  // Deletes the files now instead of when the list is destroyed.
  void remove_all() {
    std::error_code ignored;  // a file that was never created or is already renamed is fine
    for (const auto& path : paths_) std::filesystem::remove(path, ignored);
    paths_.clear();
  }

 private:
  std::vector<std::filesystem::path> paths_;
};

}  // namespace mse
