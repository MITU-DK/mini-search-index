// Paths to the fixtures in tests/data.
#pragma once

#include <filesystem>

namespace mse {

// 6 docs in 2 parts; see PLAN.md Task 0.2 for the exact content.
inline std::filesystem::path tiny_corpus_dir() {
  return std::filesystem::path(MSE_TEST_DATA_DIR) / "tiny_corpus";
}

}  // namespace mse
