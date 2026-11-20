#pragma once

#include <utility>

namespace mvcc {

// 作用域退出时执行一次，除非 Dismiss。用于"成功路径正常返回、失败路径回滚"，
// 避免在每个 return 或 catch 里重复写释放。
template <typename F>
class ScopeExit {
 public:
  explicit ScopeExit(F action) : action_(std::move(action)) {}
  ~ScopeExit() {
    if (active_) action_();
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
