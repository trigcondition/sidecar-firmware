#pragma once
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>
enum { LogNotice, LogWarning, LogError };
class CLogger {
 public:
  static CLogger* Get() { static CLogger l; return &l; }
  void Write(const char*, int, const char* fmt, ...) __attribute__((format(printf, 4, 5))) {
    char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); lines.push_back(b);
    if (getenv("TF_LOG")) printf("  log: %s\n", b);
  }
  std::vector<std::string> lines;
};
