// The `parse` loading mode (DESIGN.md §9): read the whole file and decode everything into
// ordinary containers. This is the naive baseline that mmap is compared against.
#pragma once

#include <expected>
#include <filesystem>
#include <string>

#include "inmemory_index.hpp"

namespace mse {

// Checks the whole file as it goes: a damaged file gives an error, never a crash.
[[nodiscard]] std::expected<InMemoryIndex, std::string> load_parsed(
    const std::filesystem::path& path);

}  // namespace mse
