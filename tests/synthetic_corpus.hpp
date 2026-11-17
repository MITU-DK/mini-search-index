// A reproducible random corpus for tests that need more docs than the tiny fixture.
#pragma once

#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <random>

namespace mse {

// Writes `docs` articles into `dir`, split evenly over `parts` part files. Titles are "Doc <n>"
// and words are "w0" .. "w<vocab-1>", where low numbers are far more common than high ones.
// About 1 doc in 10 has no text. The same arguments always give the same corpus.
inline void write_synthetic_corpus(const std::filesystem::path& dir, uint32_t docs, uint32_t vocab,
                                   uint32_t parts, uint32_t seed) {
  std::mt19937 rng(seed);
  const auto next_word = [&] {
    const double u = rng() / 4294967296.0;  // in [0, 1)
    return static_cast<uint32_t>(vocab * u * u * u);
  };

  for (uint32_t part = 0; part < parts; ++part) {
    std::ofstream out(dir / std::format("part-{:05}.tsv", part), std::ios::binary);
    const auto first = static_cast<uint32_t>(uint64_t{part} * docs / parts);
    const auto last = static_cast<uint32_t>(uint64_t{part + 1} * docs / parts);
    for (uint32_t doc = first; doc < last; ++doc) {
      out << "Doc " << doc << '\t';
      const uint32_t length = rng() % 10 == 0 ? 0 : 5 + rng() % 56;
      for (uint32_t i = 0; i < length; ++i) {
        if (i > 0) out << ' ';
        out << 'w' << next_word();
      }
      out << '\n';
    }
  }
}

}  // namespace mse
