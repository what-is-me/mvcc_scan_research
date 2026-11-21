#pragma once

#include <exception>
#include <utility>

namespace mvcc {

// 作用域退出时执行一次，除非 Dismiss。用于"成功路径正常返回、失败路径回滚"，
// 避免在每个 return 或 catch 里重复写释放。
template <typename F>
class ScopeExit {
 public:
  explicit ScopeExit(F action) : action_(std::move(action)) {}

  // 回滚动作不允许抛出：这个析构本身可能正在栈展开中运行，抛出去就是
  // terminate，而且此时已经没有第二次补救机会。这里接住并显式终止，让崩溃
  // 语义是确定的，而不是由运行时无诊断地杀进程。调用方要保证动作不抛。
  ~ScopeExit() noexcept {
    if (!active_) return;
    try {
      action_();
    } catch (...) {
      std::terminate();
    }
  }

  ScopeExit(const ScopeExit&) = delete;
  ScopeExit& operator=(const ScopeExit&) = delete;
  ScopeExit(ScopeExit&&) = delete;
  ScopeExit& operator=(ScopeExit&&) = delete;

  void Dismiss() { active_ = false; }

 private:
  F action_;
  bool active_ = true;
};

template <typename F>
ScopeExit<F> MakeScopeExit(F action) {
  return ScopeExit<F>(std::move(action));
}

}  // namespace mvcc
