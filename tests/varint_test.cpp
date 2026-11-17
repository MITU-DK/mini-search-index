#include "varint.hpp"

#include <gtest/gtest.h>

#include <initializer_list>
#include <limits>
#include <random>
#include <vector>

namespace mse {
namespace {

using Bytes = std::vector<std::byte>;

Bytes bytes_of(std::initializer_list<int> values) {
  Bytes out;
  for (int v : values) out.push_back(static_cast<std::byte>(v));
  return out;
}

Bytes encode_one(uint32_t value) {
  Bytes out;
  append_uleb128(value, out);
  return out;
}

TEST(Varint, KnownBytes) {
  EXPECT_EQ(encode_one(0), bytes_of({0x00}));
  EXPECT_EQ(encode_one(127), bytes_of({0x7F}));
  EXPECT_EQ(encode_one(128), bytes_of({0x80, 0x01}));
  EXPECT_EQ(encode_one(300), bytes_of({0xAC, 0x02}));
  EXPECT_EQ(encode_one(std::numeric_limits<uint32_t>::max()),
            bytes_of({0xFF, 0xFF, 0xFF, 0xFF, 0x0F}));
}

TEST(Varint, SizeMatchesTheEncoding) {
  for (uint32_t v : {0u, 1u, 127u, 128u, 16383u, 16384u, 2097151u, 2097152u, 268435455u, 268435456u,
                     std::numeric_limits<uint32_t>::max()}) {
    EXPECT_EQ(uleb128_size(v), encode_one(v).size()) << v;
  }
}

TEST(Varint, ReadValueAndAdvance) {
  const Bytes in = bytes_of({0x05, 0xAC, 0x02, 0x7F});
  std::size_t pos = 0;
  EXPECT_EQ(*read_uleb128(in, pos), 5u);
  EXPECT_EQ(*read_uleb128(in, pos), 300u);
  EXPECT_EQ(*read_uleb128(in, pos), 127u);
  EXPECT_EQ(pos, in.size());
  EXPECT_FALSE(read_uleb128(in, pos).has_value());  // nothing left
}

TEST(Varint, ReadFailsOnTruncatedOverlongAndTooWide) {
  std::size_t pos = 0;
  EXPECT_FALSE(read_uleb128(bytes_of({0x80}), pos).has_value());  // continuation, then nothing
  pos = 0;
  EXPECT_FALSE(read_uleb128(bytes_of({0x80, 0x80, 0x80, 0x80, 0x80, 0x00}), pos).has_value());
  pos = 0;  // 5 bytes, but the last one carries bits above bit 31
  EXPECT_FALSE(read_uleb128(bytes_of({0xFF, 0xFF, 0xFF, 0xFF, 0x1F}), pos).has_value());
}

// ---- postings ----

TEST(Postings, GapsAndFrequenciesAreEncodedAsPairs) {
  const std::vector<Posting> postings = {{3, 1}, {5, 2}, {200, 1}};
  Bytes out;
  encode_postings(postings, out);
  // gaps 3, 2, 195 (195 needs two bytes: C3 01), each followed by its tf
  EXPECT_EQ(out, bytes_of({0x03, 0x01, 0x02, 0x02, 0xC3, 0x01, 0x01}));
}

TEST(Postings, EncodeAppendsToWhatIsThere) {
  Bytes out = bytes_of({0xAA});
  encode_postings(std::vector<Posting>{{0, 1}}, out);
  EXPECT_EQ(out, bytes_of({0xAA, 0x00, 0x01}));
}

TEST(Postings, RoundTripRandom) {
  std::mt19937 rng(42);
  std::vector<Posting> postings;
  uint32_t doc = 0;
  for (int i = 0; i < 10000; ++i) {
    doc += 1 + (rng() % 7 == 0 ? rng() % 100000 : rng() % 20);  // mostly small gaps, some big
    const uint32_t tf = 1 + static_cast<uint32_t>(rng() % (i % 50 == 0 ? 100000 : 5));
    postings.push_back({doc, tf});
  }
  Bytes encoded;
  encode_postings(postings, encoded);
  EXPECT_LT(encoded.size(), postings.size() * 8);  // smaller than the raw u32 pairs

  std::vector<Posting> decoded;
  const auto result = decode_postings(encoded, static_cast<uint32_t>(postings.size()), decoded);
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(decoded, postings);
}

TEST(Postings, ExtremeValuesRoundTrip) {
  const uint32_t max = std::numeric_limits<uint32_t>::max();
  const std::vector<Posting> postings = {{0, max}, {1, 1}, {max, 7}};
  Bytes encoded;
  encode_postings(postings, encoded);
  std::vector<Posting> decoded;
  ASSERT_TRUE(decode_postings(encoded, 3, decoded).has_value());
  EXPECT_EQ(decoded, postings);
}

TEST(Postings, NothingEncodesToNothing) {
  Bytes encoded;
  encode_postings({}, encoded);
  EXPECT_TRUE(encoded.empty());
  std::vector<Posting> decoded;
  EXPECT_TRUE(decode_postings(encoded, 0, decoded).has_value());
  EXPECT_TRUE(decoded.empty());
}

TEST(Postings, DecodeAppendsAndKeepsTheOutputOnFailure) {
  std::vector<Posting> out = {{9, 9}};
  Bytes encoded;
  encode_postings(std::vector<Posting>{{1, 1}, {4, 2}}, encoded);
  ASSERT_TRUE(decode_postings(encoded, 2, out).has_value());
  EXPECT_EQ(out, (std::vector<Posting>{{9, 9}, {1, 1}, {4, 2}}));

  encoded.pop_back();
  EXPECT_FALSE(decode_postings(encoded, 2, out).has_value());
  EXPECT_EQ(out.size(), 3u);  // the failed call added nothing
}

TEST(Postings, TruncatedIsError) {
  Bytes encoded;
  encode_postings(std::vector<Posting>{{1, 1}, {400, 3}}, encoded);
  encoded.pop_back();
  std::vector<Posting> decoded;
  EXPECT_FALSE(decode_postings(encoded, 2, decoded).has_value());
}

TEST(Postings, OverlongIsError) {
  const Bytes six_continuation_bytes = bytes_of({0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01});
  std::vector<Posting> decoded;
  EXPECT_FALSE(decode_postings(six_continuation_bytes, 1, decoded).has_value());
}

TEST(Postings, TrailingBytesAreAnError) {
  Bytes encoded;
  encode_postings(std::vector<Posting>{{1, 1}}, encoded);
  encoded.push_back(std::byte{0});
  std::vector<Posting> decoded;
  EXPECT_FALSE(decode_postings(encoded, 1, decoded).has_value());
}

TEST(Postings, ImpossibleValuesAreErrors) {
  std::vector<Posting> decoded;
  // tf = 0 can never be written
  EXPECT_FALSE(decode_postings(bytes_of({0x01, 0x00}), 1, decoded).has_value());
  // a gap of 0 after the first posting would repeat a doc
  EXPECT_FALSE(decode_postings(bytes_of({0x01, 0x01, 0x00, 0x01}), 2, decoded).has_value());
  // doc ids that add up past 2^32 - 1
  const Bytes too_big = bytes_of({0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x01, 0x01, 0x01});
  EXPECT_FALSE(decode_postings(too_big, 2, decoded).has_value());
  EXPECT_TRUE(decoded.empty());
}

TEST(Postings, AHugeCountOnTinyInputFailsWithoutAllocating) {
  std::vector<Posting> decoded;
  EXPECT_FALSE(decode_postings(bytes_of({0x01, 0x01}), 4000000000u, decoded).has_value());
  EXPECT_TRUE(decoded.empty());
}

}  // namespace
}  // namespace mse
