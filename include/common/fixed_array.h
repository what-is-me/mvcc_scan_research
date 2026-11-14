#pragma once

#include <cstddef>
#include <memory>
#include <span>

namespace mvcc {

// 定长数组：构造时分配一次，之后没有扩容接口。元素不需要可移动。
template <typename T>
class FixedArray {
 public:
  explicit FixedArray(size_t size)
      : size_(size), data_(std::make_unique_for_overwrite<T[]>(size)) {}

  FixedArray(const FixedArray&) = delete;
  FixedArray& operator=(const FixedArray&) = delete;
  FixedArray(FixedArray&&) noexcept = default;
  FixedArray& operator=(FixedArray&&) noexcept = default;

  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }

  T& operator[](size_t index) { return data_[index]; }
  const T& operator[](size_t index) const { return data_[index]; }

  std::span<T> span() { return {data_.get(), size_}; }
  std::span<const T> span() const { return {data_.get(), size_}; }

 private:
  size_t size_;
  std::unique_ptr<T[]> data_;
};

}  // namespace mvcc
