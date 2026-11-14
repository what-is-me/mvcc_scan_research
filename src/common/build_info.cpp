#include "common/build_info.h"

namespace mvcc {
namespace {

constexpr const char* kBuildType =
#ifdef MVCC_BUILD_TYPE
    MVCC_BUILD_TYPE;
#else
#ifdef NDEBUG
    "Release";
#else
    "Debug";
#endif
#endif

}  // namespace

std::string BuildType() { return kBuildType; }

std::string Compiler() {
#if defined(__clang__)
  return std::string("Clang ") + __clang_version__;
#elif defined(_MSC_VER)
  return "MSVC " + std::to_string(_MSC_VER / 100) + "." + std::to_string(_MSC_VER % 100);
#elif defined(__GNUC__)
  return std::string("GCC ") + __VERSION__;
#else
  return "unknown";
#endif
}

std::string OperatingSystem() {
#if defined(_WIN32)
  return sizeof(void*) == 8 ? "Windows x64" : "Windows x86";
#elif defined(__linux__)
  return sizeof(void*) == 8 ? "Linux x64" : "Linux x86";
#elif defined(__APPLE__)
  return sizeof(void*) == 8 ? "macOS arm64" : "macOS x86";
#else
  return "unknown";
#endif
}

std::string BuildSummary() {
  std::string summary = "build=" + BuildType();
  summary += " compiler=" + Compiler();
  summary += " os=" + OperatingSystem();
#ifdef NDEBUG
  summary += " asserts=off";
#else
  summary += " asserts=on";
#endif
  summary += " cxx=" + std::to_string(__cplusplus);
  return summary;
}

}  // namespace mvcc
