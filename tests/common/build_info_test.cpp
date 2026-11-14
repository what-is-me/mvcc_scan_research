#include <string>

#include <gtest/gtest.h>

#include "common/build_info.h"

TEST(BuildInfo, ReportsEveryField) {
  EXPECT_FALSE(mvcc::BuildType().empty());
  EXPECT_FALSE(mvcc::Compiler().empty());
  EXPECT_FALSE(mvcc::OperatingSystem().empty());

  const std::string summary = mvcc::BuildSummary();
  EXPECT_NE(summary.find("build="), std::string::npos);
  EXPECT_NE(summary.find("compiler="), std::string::npos);
  EXPECT_NE(summary.find("os="), std::string::npos);
  EXPECT_NE(summary.find("asserts="), std::string::npos);
  EXPECT_NE(summary.find("cxx="), std::string::npos);
}

TEST(BuildInfo, BuildTypeComesFromTheBuildSystem) {
  const std::string build_type = mvcc::BuildType();
  const bool known = build_type == "Debug" || build_type == "Release" ||
                     build_type == "RelWithDebInfo" || build_type == "MinSizeRel";
  EXPECT_TRUE(known) << "unexpected build type: " << build_type;
}
