#pragma once

#include "buffer/frame.h"

namespace mvcc {

// 在作用域内持有一次 pin，析构时归还。用于所有 "Fix 之后有多个 return"
// 的路径：把重复的 Unpin 收敛到析构，异常路径也不会漏 pin。
template <typename Pool>
class PinGuard {
 public:
  PinGuard(Pool& pool, Frame* frame) : pool_(&pool), frame_(frame) {}
  ~PinGuard() { pool_->Unpin(frame_); }

  PinGuard(const PinGuard&) = delete;
  PinGuard& operator=(const PinGuard&) = delete;
  PinGuard(PinGuard&&) = delete;
  PinGuard& operator=(PinGuard&&) = delete;

 private:
  Pool* pool_;
  Frame* frame_;
};

}  // namespace mvcc
