#pragma once

#include <cstddef>

namespace mvcc {

// 无缓冲 I/O 要求偏移、长度与缓冲区地址对齐到的字节数。
inline constexpr size_t kSectorBytes = 4096;

// 文件读写是否绕过系统页缓存。
// kBuffered：走系统页缓存，未命中时只做一次内存拷贝，读到的延迟与存储设备无关。
// kDirect：Windows 用 FILE_FLAG_NO_BUFFERING、POSIX 用 O_DIRECT，每次未命中都到设备；
//          要求文件偏移、传输长度与缓冲区地址都按扇区（4096 B）对齐。
enum class IoMode { kBuffered, kDirect };

}  // namespace mvcc
