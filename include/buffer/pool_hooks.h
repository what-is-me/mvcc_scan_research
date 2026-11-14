#pragma once

namespace mvcc {

enum class PoolHookPoint { kBeforeWriteBack, kBeforeFileRead, kAfterFileRead };

// 确定性交错注入点：生产路径为 nullptr，代价是一次可预测分支。
class PoolHooks {
 public:
  PoolHooks() = default;
  PoolHooks(void (*notify)(void*, PoolHookPoint), void* context)
      : notify_(notify), context_(context) {}

  void Fire(PoolHookPoint point) const {
    if (notify_ != nullptr) notify_(context_, point);
  }

 private:
  void (*notify_)(void*, PoolHookPoint) = nullptr;
  void* context_ = nullptr;
};

}  // namespace mvcc
