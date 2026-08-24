#pragma once
#include "unified_buffer/runtime.hpp"
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// Small shared helpers for the runnable examples.  Header-only; every inline
// function keeps each example self-contained and single-TU safe.
namespace ubex {

inline void print(const std::string& line) { std::cout << line << std::endl; }

// Human-readable error string from a Result (works for any Result<T> via
// metaprogramming-free overloads on the common members).
template <class T>
inline std::string errstr(const unified_buffer::Result<T>& r) {
  return std::string(unified_buffer::to_string(r.error())) + ": " + r.message();
}

inline std::string hex32(std::uint32_t x) {
  std::ostringstream os;
  os << std::hex << std::setw(8) << std::setfill('0') << x;
  return os.str();
}

// True if the memory domain is enabled for this runtime.
inline bool domain_enabled(const unified_buffer::Runtime& rt, unified_buffer::MemoryDomain d) {
  auto ds = rt.domains();
  if (!ds.ok()) return false;
  for (const auto& de : ds.value()) {
    if (de.domain == d) return de.enabled;
  }
  return false;
}

// True if at least one CUDA device is exposed by this runtime.
inline bool has_cuda(const unified_buffer::Runtime& rt) {
  auto devs = rt.devices();
  if (!devs.ok()) return false;
  return !devs.value().empty();
}

} // namespace ubex
