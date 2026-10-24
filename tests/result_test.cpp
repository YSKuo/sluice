#include "result.h"

#include <cerrno>
#include <memory>
#include <system_error>
#include <utility>

#include "gtest/gtest.h"

namespace sluice {
namespace {

Result<int> Half(int n) {
  if (n % 2 != 0) {
    return ErrnoError(EINVAL);
  }
  return n / 2;
}

TEST(ResultTest, HoldsValue) {
  const Result<int> result = Half(10);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(static_cast<bool>(result));
  EXPECT_EQ(*result, 5);
}

TEST(ResultTest, HoldsError) {
  const Result<int> result = Half(3);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), std::errc::invalid_argument);
}

TEST(ResultTest, ValueOr) {
  EXPECT_EQ(Half(10).value_or(-1), 5);
  EXPECT_EQ(Half(3).value_or(-1), -1);
}

TEST(ResultTest, SupportsMoveOnlyValues) {
  Result<std::unique_ptr<int>> result = std::make_unique<int>(7);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(**result, 7);
  const std::unique_ptr<int> taken = *std::move(result);
  EXPECT_EQ(*taken, 7);
}

TEST(ResultTest, ArrowAccessesMembers) {
  struct Point {
    int x;
  };
  const Result<Point> result = Point{.x = 3};
  EXPECT_EQ(result->x, 3);
}

TEST(ResultTest, VoidSuccessAndFailure) {
  const Result<void> ok;
  EXPECT_TRUE(ok.has_value());

  const Result<void> failed = ErrnoError(EADDRINUSE);
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(failed.error(), std::errc::address_in_use);
}

TEST(ResultTest, IsWouldBlock) {
  EXPECT_TRUE(IsWouldBlock(ErrnoError(EAGAIN).code()));
  EXPECT_TRUE(IsWouldBlock(ErrnoError(EWOULDBLOCK).code()));
  EXPECT_FALSE(IsWouldBlock(ErrnoError(ECONNRESET).code()));
}

}  // namespace
}  // namespace sluice
