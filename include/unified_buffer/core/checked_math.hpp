#pragma once
#include <cstdint>
#include <cstddef>
#include <limits>
#include <optional>

namespace unified_buffer {

// Overflow-safe arithmetic helpers.  These are the single source of truth for
// every size / offset computation in the runtime.  Backends and slices must use
// them; raw signed or unchecked arithmetic is not permitted on metadata paths.

constexpr std::size_t kMaxSizeT = std::numeric_limits<std::size_t>::max();

// Does a + b overflow size_t?  Returns false if it does (i.e. callers must not
// use the result).
[[nodiscard]] inline bool add_overflow(std::size_t a, std::size_t b, std::size_t& out) noexcept {
  if (b > kMaxSizeT - a) return true;
  out = a + b;
  return false;
}

// Does a * b overflow size_t?
[[nodiscard]] inline bool mul_overflow(std::size_t a, std::size_t b, std::size_t& out) noexcept {
  if (a == 0 || b == 0) { out = 0; return false; }
  if (a > kMaxSizeT / b) return true;
  out = a * b;
  return false;
}

// Round a value up to a power-of-two alignment.  Returns nullopt on overflow.
// alignment must be a power of two.
[[nodiscard]] inline std::optional<std::size_t> align_up(std::size_t value, std::size_t alignment) noexcept {
  if (alignment == 0) return std::nullopt;
  if ((alignment & (alignment - 1)) != 0) return std::nullopt; // not a power of two
  const std::size_t mask = alignment - 1;
  if (value > kMaxSizeT - mask) return std::nullopt;
  return (value + mask) & ~mask;
}

// Is value aligned to a power-of-two alignment?
[[nodiscard]] inline bool is_aligned(std::size_t value, std::size_t alignment) noexcept {
  if (alignment == 0) return false;
  if ((alignment & (alignment - 1)) != 0) return false;
  return (value & (alignment - 1)) == 0;
}

// Round a value DOWN to a power-of-two alignment.
[[nodiscard]] inline std::size_t align_down(std::size_t value, std::size_t alignment) noexcept {
  if (alignment == 0) return value;
  return value & ~(alignment - 1);
}

// Does the range [offset, offset+length) fit within [0, total)?
[[nodiscard]] inline bool range_in_bounds(std::size_t offset, std::size_t length, std::size_t total) noexcept {
  std::size_t end = 0;
  if (add_overflow(offset, length, end)) return false;
  return end <= total;
}

} // namespace unified_buffer
