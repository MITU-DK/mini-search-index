#include "varint.hpp"

#include <cassert>

namespace mse {
namespace {

constexpr unsigned kPayloadBits = 7;
constexpr uint32_t kPayloadMask = 0x7F;
constexpr uint8_t kContinue = 0x80;
constexpr std::size_t kMaxBytes = 5;
constexpr uint8_t kLastByteMax = 0x0F;  // the 5th byte holds bits 28..31 only

// Every posting is at least two bytes (one per number), which bounds how many a block can hold.
constexpr std::size_t kMinBytesPerPosting = 2;

}  // namespace

std::size_t uleb128_size(uint32_t value) {
  std::size_t size = 1;
  while (value > kPayloadMask) {
    value >>= kPayloadBits;
    ++size;
  }
  return size;
}

void append_uleb128(uint32_t value, std::vector<std::byte>& out) {
  while (value > kPayloadMask) {
    out.push_back(static_cast<std::byte>((value & kPayloadMask) | kContinue));
    value >>= kPayloadBits;
  }
  out.push_back(static_cast<std::byte>(value));
}

std::expected<uint32_t, std::string> read_uleb128(std::span<const std::byte> in, std::size_t& pos) {
  uint32_t value = 0;
  for (std::size_t i = 0; i < kMaxBytes; ++i) {
    if (pos >= in.size()) return std::unexpected("number is cut off by the end of the data");
    const auto byte = static_cast<uint8_t>(in[pos++]);
    if (i == kMaxBytes - 1 && byte > kLastByteMax) {
      return std::unexpected("number does not fit in 32 bits");
    }
    value |= static_cast<uint32_t>(byte & kPayloadMask) << (kPayloadBits * i);
    if ((byte & kContinue) == 0) return value;
  }
  return std::unexpected("number is longer than 5 bytes");
}

void encode_postings(std::span<const Posting> postings, std::vector<std::byte>& out) {
  uint32_t previous = 0;
  for (std::size_t i = 0; i < postings.size(); ++i) {
    assert(i == 0 || postings[i].doc > postings[i - 1].doc);
    assert(postings[i].tf >= 1);
    append_uleb128(postings[i].doc - previous, out);
    append_uleb128(postings[i].tf, out);
    previous = postings[i].doc;
  }
}

std::expected<void, std::string> decode_postings(std::span<const std::byte> in, uint32_t count,
                                                 std::vector<Posting>& out) {
  if (count > in.size() / kMinBytesPerPosting) {
    return std::unexpected("postings block is too short for its posting count");
  }

  const std::size_t original_size = out.size();
  const auto fail = [&](const std::string& why) -> std::expected<void, std::string> {
    out.resize(original_size);
    return std::unexpected(why);
  };

  out.reserve(original_size + count);
  std::size_t pos = 0;
  uint64_t doc = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const auto gap = read_uleb128(in, pos);
    if (!gap) return fail(gap.error());
    const auto tf = read_uleb128(in, pos);
    if (!tf) return fail(tf.error());
    if (i > 0 && *gap == 0) return fail("a doc is listed twice");
    if (*tf == 0) return fail("a posting has a frequency of 0");
    doc += *gap;
    if (doc > UINT32_MAX) return fail("doc id does not fit in 32 bits");
    out.push_back({static_cast<uint32_t>(doc), *tf});
  }
  if (pos != in.size()) return fail("postings block has bytes left after its last posting");
  return {};
}

}  // namespace mse
