#include "common/config.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <system_error>

#include "common/aligned_buffer.h"

namespace mvcc {
namespace {

constexpr size_t kRowHeaderBytes = 16;
constexpr size_t kMaxRowsPerPage = 65536;

// undo 无 GC，每条更新留一条行宽的记录，容量必须覆盖整个实验时长（§11）。
// 速率取 100 万行/秒：本机 TP 实测在 10 万行/秒量级（内存文件、零 I/O 等待），
// 留一个数量级的余量给更快的机器和更多线程。启动就报错，不要跑一半写满磁盘。
constexpr uint64_t kMaxUpdatesPerSecond = 1'000'000;
constexpr uint64_t kGiB = 1024ull * 1024 * 1024;

// 上界只为让后续乘法不溢出：这些值远超实验矩阵，超了就不是配置错误而是
// 输入错误，启动即拒。
constexpr size_t kMaxTableRows = 1'000'000'000;
constexpr size_t kMaxPoolMb = 1'048'576;
constexpr size_t kMaxDurationSeconds = 86'400;

std::string_view Trim(std::string_view text) {
  const size_t begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string_view::npos) return {};
  const size_t end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1);
}

bool ParseSize(std::string_view text, size_t* out) {
  if (text.empty()) return false;
  size_t value = 0;
  const char* last = text.data() + text.size();
  const std::from_chars_result result = std::from_chars(text.data(), last, value);
  if (result.ec != std::errc() || result.ptr != last) return false;
  *out = value;
  return true;
}

template <typename E>
struct EnumEntry {
  const char* name;
  E value;
};

constexpr EnumEntry<KeyDist> kKeyDistEntries[] = {
    {"uniform", KeyDist::kUniform},
    {"zipf0.5", KeyDist::kZipf05},
    {"zipf0.8", KeyDist::kZipf08},
    {"zipf1.1", KeyDist::kZipf11},
};

constexpr EnumEntry<KeyMapping> kKeyMappingEntries[] = {
    {"scattered", KeyMapping::kScattered},
    {"head", KeyMapping::kHead},
};

constexpr EnumEntry<ScanAccessMode> kScanAccessModeEntries[] = {
    {"scan", ScanAccessMode::kScan},
    {"normal", ScanAccessMode::kNormal},
};

constexpr EnumEntry<ScanPolicy> kScanPolicyEntries[] = {
    {"baseline", ScanPolicy::kBaseline},
    {"hotfirst", ScanPolicy::kHotFirst},
    {"defer_chain", ScanPolicy::kDeferChain},
    {"defer_paged", ScanPolicy::kDeferPaged},
};

constexpr EnumEntry<Replacer> kReplacerEntries[] = {
    {"lru", Replacer::kLru},
    {"clock", Replacer::kClock},
    {"mru", Replacer::kMru},
    {"arc", Replacer::kArc},
    {"lru_k", Replacer::kLruK},
};

template <typename E, size_t N>
bool ParseEnum(const EnumEntry<E> (&entries)[N], std::string_view text, E* out) {
  for (const EnumEntry<E>& entry : entries) {
    if (text == entry.name) {
      *out = entry.value;
      return true;
    }
  }
  return false;
}

template <typename E, size_t N>
const char* EnumName(const EnumEntry<E> (&entries)[N], E value) {
  for (const EnumEntry<E>& entry : entries) {
    if (entry.value == value) return entry.name;
  }
  return "unknown";
}

}  // namespace

const char* ToString(KeyDist value) { return EnumName(kKeyDistEntries, value); }
const char* ToString(KeyMapping value) { return EnumName(kKeyMappingEntries, value); }
const char* ToString(ScanAccessMode value) { return EnumName(kScanAccessModeEntries, value); }
const char* ToString(ScanPolicy value) { return EnumName(kScanPolicyEntries, value); }
const char* ToString(Replacer value) { return EnumName(kReplacerEntries, value); }

bool Config::Set(std::string_view key, std::string_view value, std::string* error) {
  const auto invalid = [&](std::string_view expected) {
    *error = "invalid value for '" + std::string(key) + "': '" + std::string(value) +
             "', expected " + std::string(expected);
    return false;
  };

  if (key == "page_size_kb") {
    if (!ParseSize(value, &page_size_kb)) return invalid("integer");
    return true;
  }
  if (key == "row_size_b") {
    if (!ParseSize(value, &row_size_b)) return invalid("integer");
    return true;
  }
  if (key == "table_rows") {
    if (!ParseSize(value, &table_rows)) return invalid("integer");
    return true;
  }
  if (key == "base_pool_mb") {
    if (!ParseSize(value, &base_pool_mb)) return invalid("integer");
    return true;
  }
  if (key == "undo_pool_mb") {
    if (!ParseSize(value, &undo_pool_mb)) return invalid("integer");
    return true;
  }
  if (key == "undo_capacity_gb") {
    if (!ParseSize(value, &undo_capacity_gb)) return invalid("integer");
    return true;
  }
  if (key == "tp_threads") {
    if (!ParseSize(value, &tp_threads)) return invalid("integer");
    return true;
  }
  if (key == "key_dist") {
    if (!ParseEnum(kKeyDistEntries, value, &key_dist)) {
      return invalid("uniform | zipf0.5 | zipf0.8 | zipf1.1");
    }
    return true;
  }
  if (key == "key_mapping") {
    if (!ParseEnum(kKeyMappingEntries, value, &key_mapping)) return invalid("scattered | head");
    return true;
  }
  if (key == "rows_per_txn") {
    if (!ParseSize(value, &rows_per_txn)) return invalid("integer");
    return true;
  }
  if (key == "scan_policy") {
    if (!ParseEnum(kScanPolicyEntries, value, &scan_policy)) {
      return invalid("baseline | hotfirst | defer_chain | defer_paged");
    }
    return true;
  }
  if (key == "scan_interval_ms") {
    if (!ParseSize(value, &scan_interval_ms)) return invalid("integer");
    return true;
  }
  if (key == "scan_access_mode") {
    if (!ParseEnum(kScanAccessModeEntries, value, &scan_access_mode)) {
      return invalid("scan | normal");
    }
    return true;
  }
  if (key == "replacer") {
    if (!ParseEnum(kReplacerEntries, value, &replacer)) {
      return invalid("lru | clock | mru | arc | lru_k");
    }
    return true;
  }
  if (key == "duration_s") {
    if (!ParseSize(value, &duration_s)) return invalid("integer");
    return true;
  }
  if (key == "results_dir") {
    if (value.empty()) return invalid("non-empty path");
    results_dir = std::string(value);
    return true;
  }

  *error = "unknown key '" + std::string(key) + "'";
  return false;
}

std::string Config::Validate() const {
  if (page_size_kb == 0) return "page_size_kb must be positive";
  if (row_size_b < kRowHeaderBytes) return "row_size_b must hold a 16-byte row header";
  // 数据区至少要放一个折叠字。行宽恰好等于头长时数据区为空，校验和对任何
  // 内容都相同，正确性 oracle 直接失效——这种配置必须启动即拒。
  if (row_size_b < kRowHeaderBytes + sizeof(uint64_t)) {
    return "row_size_b must leave at least 8 bytes of row data";
  }
  if (row_size_b % 8 != 0) return "row_size_b must be a multiple of 8";
  if (table_rows == 0) return "table_rows must be positive";
  if (table_rows > kMaxTableRows) return "table_rows is beyond the supported range";

  const size_t page_bytes = page_size_kb * 1024;
  if (row_size_b > page_bytes) return "row_size_b must not exceed page size";
  if (page_bytes / row_size_b > kMaxRowsPerPage) return "too many rows per page for a 16-bit slot";
  if (page_bytes % kSectorBytes != 0) {
    return "page_size_kb must be a multiple of 4 for direct I/O";
  }

  if (base_pool_mb == 0) return "base_pool_mb must be positive";
  if (undo_pool_mb == 0) return "undo_pool_mb must be positive";
  if (undo_capacity_gb == 0) return "undo_capacity_gb must be positive";
  if (base_pool_mb > kMaxPoolMb) return "base_pool_mb is beyond the supported range";
  if (undo_pool_mb > kMaxPoolMb) return "undo_pool_mb is beyond the supported range";

  const uint64_t undo_required = static_cast<uint64_t>(duration_s) * kMaxUpdatesPerSecond *
                                 static_cast<uint64_t>(row_size_b);
  if (static_cast<uint64_t>(undo_capacity_gb) * kGiB < undo_required) {
    const uint64_t required_gb = (undo_required + kGiB - 1) / kGiB;
    return "undo_capacity_gb must be at least " + std::to_string(required_gb) +
           " for duration_s=" + std::to_string(duration_s) +
           " (estimated at 1M updates/s); raise it or shorten the run";
  }

  if (tp_threads == 0) return "tp_threads must be positive";
  if (rows_per_txn == 0) return "rows_per_txn must be positive";
  if (scan_interval_ms == 0) return "scan_interval_ms must be positive";
  if (duration_s == 0) return "duration_s must be positive";
  if (duration_s > kMaxDurationSeconds) return "duration_s is beyond the supported range";

  const size_t base_frames = base_pool_mb * 1024 * 1024 / page_bytes;
  if (rows_per_txn > base_frames) {
    return "rows_per_txn must not exceed the base pool frame count (" +
           std::to_string(base_frames) + ")";
  }

  // 提交时逐行 pin 写集页。写集比池还大时，多个事务会各自攥着一部分帧等
  // 下一页，谁也放不掉——必须在启动时不接受，而不是跑到一半挂住。
  return {};
}

std::vector<std::string> Config::Warnings() const {
  std::vector<std::string> warnings;
  const size_t table_bytes = table_rows * row_size_b;
  if (base_pool_mb * 1024 * 1024 >= table_bytes) {
    warnings.push_back(
        "base pool covers the whole table: scans never evict, replacer differences disappear");
  }
  return warnings;
}

std::string Config::Describe() const {
  std::string text = "page_size_kb=" + std::to_string(page_size_kb);
  text += " row_size_b=" + std::to_string(row_size_b);
  text += " table_rows=" + std::to_string(table_rows);
  text += " base_pool_mb=" + std::to_string(base_pool_mb);
  text += " undo_pool_mb=" + std::to_string(undo_pool_mb);
  text += " undo_capacity_gb=" + std::to_string(undo_capacity_gb);
  text += " tp_threads=" + std::to_string(tp_threads);
  text += " key_dist=" + std::string(ToString(key_dist));
  text += " key_mapping=" + std::string(ToString(key_mapping));
  text += " rows_per_txn=" + std::to_string(rows_per_txn);
  text += " scan_policy=" + std::string(ToString(scan_policy));
  text += " scan_interval_ms=" + std::to_string(scan_interval_ms);
  text += " scan_access_mode=" + std::string(ToString(scan_access_mode));
  text += " replacer=" + std::string(ToString(replacer));
  text += " duration_s=" + std::to_string(duration_s);
  text += " results_dir=" + results_dir;
  return text;
}

bool LoadConfigFile(const std::string& path, Config* config, std::string* error) {
  std::ifstream in(path);
  if (!in) {
    *error = "cannot open config file: " + path;
    return false;
  }

  std::string line;
  size_t line_no = 0;
  while (std::getline(in, line)) {
    ++line_no;
    std::string_view view = line;
    const size_t comment = view.find('#');
    if (comment != std::string_view::npos) view = view.substr(0, comment);
    view = Trim(view);
    if (view.empty()) continue;

    const size_t equals = view.find('=');
    if (equals == std::string_view::npos) {
      *error = path + ":" + std::to_string(line_no) + ": expected 'key = value'";
      return false;
    }

    std::string detail;
    if (!config->Set(Trim(view.substr(0, equals)), Trim(view.substr(equals + 1)), &detail)) {
      *error = path + ":" + std::to_string(line_no) + ": " + detail;
      return false;
    }
  }
  return true;
}

}  // namespace mvcc
