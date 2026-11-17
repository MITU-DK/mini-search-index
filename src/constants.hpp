// Project-wide constants and platform checks. Values come from DESIGN.md §4–§8.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>

static_assert(std::endian::native == std::endian::little, "index format assumes little-endian");
static_assert(sizeof(void*) == 8, "64-bit platforms only");

namespace mse {

// Tokenizer (§6)
inline constexpr std::size_t kMaxTokenLen = 64;

// Ranking (§7)
inline constexpr float kBm25K1 = 1.2f;
inline constexpr float kBm25B = 0.75f;
inline constexpr std::size_t kDefaultTopK = 10;

// Indexing: docs per work batch handed to a thread
inline constexpr std::size_t kBatchSize = 256;

// Index file format (§8)
inline constexpr std::size_t kHeaderSize = 256;
inline constexpr std::size_t kSectionAlign = 8;

// Autocomplete
inline constexpr std::size_t kSuggestMinPrefix = 2;
inline constexpr std::size_t kSuggestTopK = 5;

// Spell check: only terms seen in at least this many docs can be suggested
inline constexpr std::uint32_t kSpellMinDf = 3;

// Snippets: window size, and how many bytes to show before the first match
inline constexpr std::size_t kSnippetWindow = 160;
inline constexpr std::size_t kSnippetLead = 40;

}  // namespace mse
