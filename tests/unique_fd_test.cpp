#include "net/unique_fd.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <utility>

namespace sluice::net {
namespace {

bool IsOpen(int fd) { return ::fcntl(fd, F_GETFD) != -1 || errno != EBADF; }

// Returns {read end, write end} of a fresh pipe.
std::pair<int, int> MakePipe() {
  std::array<int, 2> fds{};
  EXPECT_EQ(::pipe2(fds.data(), O_CLOEXEC), 0);
  return {fds[0], fds[1]};
}

TEST(UniqueFdTest, DefaultIsInvalid) {
  const UniqueFd fd;
  EXPECT_FALSE(fd.valid());
  EXPECT_EQ(fd.get(), -1);
}

TEST(UniqueFdTest, ClosesOnDestruction) {
  auto [r, w] = MakePipe();
  {
    const UniqueFd read_end(r);
    const UniqueFd write_end(w);
    EXPECT_TRUE(IsOpen(r));
  }
  EXPECT_FALSE(IsOpen(r));
  EXPECT_FALSE(IsOpen(w));
}

TEST(UniqueFdTest, MoveTransfersOwnership) {
  auto [r, w] = MakePipe();
  const UniqueFd write_end(w);
  UniqueFd a(r);
  UniqueFd b(std::move(a));
  EXPECT_FALSE(a.valid());  // NOLINT(bugprone-use-after-move): checking moved-from state.
  EXPECT_EQ(b.get(), r);

  UniqueFd c;
  c = std::move(b);
  EXPECT_EQ(c.get(), r);
  EXPECT_TRUE(IsOpen(r));
}

TEST(UniqueFdTest, MoveAssignmentClosesPreviousDescriptor) {
  auto [r1, w1] = MakePipe();
  auto [r2, w2] = MakePipe();
  const UniqueFd w1_owner(w1);
  const UniqueFd w2_owner(w2);
  UniqueFd target(r1);
  UniqueFd source(r2);
  target = std::move(source);
  EXPECT_FALSE(IsOpen(r1));
  EXPECT_EQ(target.get(), r2);
}

TEST(UniqueFdTest, ReleaseGivesUpOwnership) {
  auto [r, w] = MakePipe();
  const UniqueFd write_end(w);
  int released = -1;
  {
    UniqueFd fd(r);
    released = fd.Release();
    EXPECT_FALSE(fd.valid());
  }
  EXPECT_TRUE(IsOpen(released));
  ::close(released);
}

TEST(UniqueFdTest, ResetClosesAndAdopts) {
  auto [r, w] = MakePipe();
  UniqueFd fd(r);
  fd.Reset(w);
  EXPECT_FALSE(IsOpen(r));
  EXPECT_EQ(fd.get(), w);
  fd.Reset();
  EXPECT_FALSE(IsOpen(w));
  EXPECT_FALSE(fd.valid());
}

}  // namespace
}  // namespace sluice::net
