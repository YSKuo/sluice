#include "gtest/gtest.h"

#include "version.h"

TEST(Smoke, VersionIsNotEmpty) { EXPECT_FALSE(sluice::GetVersion().empty()); }
