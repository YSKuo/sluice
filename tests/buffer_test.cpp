#include "net/buffer.h"

#include <cstring>
#include <span>
#include <string>

#include "gtest/gtest.h"

namespace sluice::net {
namespace {

TEST(BufferTest, StartsEmpty) {
  const Buffer buf;
  EXPECT_TRUE(buf.empty());
  EXPECT_EQ(buf.ReadableBytes(), 0U);
  EXPECT_EQ(buf.Peek(), "");
}

TEST(BufferTest, AppendThenRetrieve) {
  Buffer buf;
  buf.Append("hello ");
  buf.Append("world");
  EXPECT_EQ(buf.Peek(), "hello world");

  buf.Retrieve(6);
  EXPECT_EQ(buf.Peek(), "world");

  EXPECT_EQ(buf.RetrieveAllAsString(), "world");
  EXPECT_TRUE(buf.empty());
}

TEST(BufferTest, RetrieveMoreThanReadableEmptiesBuffer) {
  Buffer buf;
  buf.Append("abc");
  buf.Retrieve(100);
  EXPECT_TRUE(buf.empty());
}

TEST(BufferTest, AppendEmptyIsNoOp) {
  Buffer buf;
  buf.Append("");
  EXPECT_TRUE(buf.empty());
}

TEST(BufferTest, PrepareAndCommitWrite) {
  Buffer buf;
  std::span<char> space = buf.PrepareWrite(16);
  ASSERT_GE(space.size(), 16U);
  std::memcpy(space.data(), "abcd", 4);
  buf.CommitWrite(4);
  EXPECT_EQ(buf.Peek(), "abcd");
}

TEST(BufferTest, ReclaimsConsumedPrefixBeforeGrowing) {
  Buffer buf;
  buf.Append(std::string(100, 'x'));
  buf.Retrieve(90);
  // 10 readable bytes remain; asking for 90 should fit by moving them to
  // the front rather than by growing.
  const std::span<char> space = buf.PrepareWrite(90);
  EXPECT_EQ(space.size(), 90U);
  EXPECT_EQ(buf.Peek(), std::string(10, 'x'));
}

TEST(BufferTest, GrowsForLargeAppends) {
  Buffer buf;
  std::string big(1 << 20, '\0');
  for (size_t i = 0; i < big.size(); ++i) {
    big[i] = static_cast<char>(i % 251);
  }
  buf.Append("prefix");
  buf.Append(big);
  buf.Retrieve(6);
  EXPECT_EQ(buf.Peek(), big);
}

}  // namespace
}  // namespace sluice::net
