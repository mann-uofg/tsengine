#include <gtest/gtest.h>

#include "tsengine/version.h"

// Proves the build, test framework and CI are wired up. Delete once real tests exist.
TEST(Smoke, VersionIsSet) { EXPECT_STREQ(tsengine::Version(), "0.1.0"); }
