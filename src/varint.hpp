// Gap + VByte coding of postings lists (DESIGN.md §8).
//
// A number is written 7 bits at a time, lowest bits first; every byte but the last has its top
// bit set (unsigned LEB128). Small numbers take 1 byte, and doc ids stored as gaps are small.
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "posting.hpp"

namespace mse {

// The number of bytes `value` takes: 1 to 5.
[[nodiscard]] std::size_t uleb128_size(uint32_t value);

void append_uleb128(uint32_t value, std::vector<std::byte>& out);

// Reads one number at `pos` and moves `pos` past it. Fails on truncated input and on a number
// that does not fit in 32 bits (more than 5 bytes, or a 5th byte above 0x0F).
[[nodiscard]] std::expected<uint32_t, std::string> read_uleb128(std::span<const std::byte> in,
                                                                std::size_t& pos);

// Appends {gap, tf} for each posting; the first gap is the doc id itself (the previous doc
// starts at 0). Docs must be strictly increasing and tf >= 1.
void encode_postings(std::span<const Posting> postings, std::vector<std::byte>& out);

// Appends `count` postings decoded from all of `in`. Fails, leaving `out` as it was, when the
// input is truncated, has bytes left over, or holds a value that no valid list can contain
// (tf = 0, a repeated doc, a doc id past 2^32 - 1).
[[nodiscard]] std::expected<void, std::string> decode_postings(std::span<const std::byte> in,
                                                               uint32_t count,
                                                               std::vector<Posting>& out);

}  // namespace mse
