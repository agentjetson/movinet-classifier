#pragma once
#include <cstdlib>
#include <string>

namespace edge {

inline std::string getenv_or(const char* key, const char* fallback) {
  if (const char* v = std::getenv(key); v && *v) return v;
  return fallback;
}

}  // namespace edge
