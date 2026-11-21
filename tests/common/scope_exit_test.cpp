#include <stdexcept>

#include <gtest/gtest.h>

#include "common/scope_exit.h"

namespace mvcc {
namespace {

TEST(ScopeExit, RunsWhenTheScopeEnds) {
  int runs = 0;
  {
    const auto guard = MakeScopeExit([&runs] { ++runs; });
    EXPECT_EQ(runs, 0);
  }
  EXPECT_EQ(runs, 1);
}

TEST(ScopeExit, DoesNothingAfterDismiss) {
  int runs = 0;
  {
    auto guard = MakeScopeExit([&runs] { ++runs; });
    guard.Dismiss();
  }
  EXPECT_EQ(runs, 0);
}

// 回滚动作最主要的用途就是异常路径，栈展开时它必须照常执行。
TEST(ScopeExit, RunsWhileTheStackUnwinds) {
  int runs = 0;
  try {
    const auto guard = MakeScopeExit([&runs] { ++runs; });
    throw std::runtime_error("unwind");
  } catch (const std::runtime_error&) {
  }
  EXPECT_EQ(runs, 1);
}

}  // namespace
}  // namespace mvcc
