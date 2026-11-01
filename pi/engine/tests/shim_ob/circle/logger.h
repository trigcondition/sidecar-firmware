#pragma once
#include <circle/types.h>
#include <string>
#include <vector>
enum { LogNotice, LogWarning, LogError };
class CLogger {
 public:
  static CLogger* Get();
  void Write(const char* from, int sev, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
  std::vector<std::string> lines;
};
