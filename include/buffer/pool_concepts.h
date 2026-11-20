#pragma once

#include <concepts>
#include <cstddef>
#include <optional>
#include <span>

#include "buffer/frame.h"
#include "common/basic_types.h"

namespace mvcc {

// 页号 -> 帧下标的映射。Find 在池的元数据锁内调用，也用于命中路径判定；
// Insert/Erase 只在元数据锁内调用。ForEach 供不变量检查遍历有效条目。
template <typename M>
concept PageMapping = requires(M mapping, const M& const_mapping, PageId page, size_t frame) {
  { const_mapping.Find(page) } -> std::same_as<std::optional<size_t>>;
  { mapping.Insert(page, frame) } -> std::same_as<void>;
  { mapping.Erase(page) } -> std::same_as<void>;
  { const_mapping.ForEach([](PageId, size_t) {}) } -> std::same_as<void>;
};

// 替换策略。装入选帧时给新近性提示、命中时更新新近性、淘汰时摘除候选资格；
// 选牺牲帧只从 pin 为 0 的帧里挑，挑不出返回 nullopt。全部调用都在池的元数据
// 锁内，策略自身不必考虑并发。
//
// 每个回调都带页号：ARC 的幽灵链表按页号记录被淘汰的页，命中幽灵时要调整
// T1/T2 的目标大小，只靠帧下标做不到（帧会复用给别的页）。
template <typename R>
concept FrameReplacer =
    requires(R replacer, const R& const_replacer, size_t frame, PageId page, AccessMode mode,
             std::span<const Frame> frames) {
      { replacer.OnHit(frame, page, mode) } -> std::same_as<void>;
      { replacer.OnLoad(frame, page, mode) } -> std::same_as<void>;
      { replacer.OnEvict(frame, page) } -> std::same_as<void>;
      { const_replacer.SelectVictim(page, frames) } -> std::same_as<std::optional<size_t>>;
    };

}  // namespace mvcc
