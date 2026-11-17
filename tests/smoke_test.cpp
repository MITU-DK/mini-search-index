#include <gtest/gtest.h>

#include "constants.hpp"

TEST(Smoke, ConstantsVisible) { EXPECT_EQ(mse::kMaxTokenLen, 64u); }
