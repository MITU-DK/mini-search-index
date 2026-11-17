// Test helpers: whole files as byte vectors.
#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace mse {

using Bytes = std::vector<std::byte>;

inline Bytes read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const auto* first = reinterpret_cast<const std::byte*>(text.data());
  return Bytes(first, first + text.size());
}

inline void write_file(const std::filesystem::path& path, const Bytes& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

}  // namespace mse
