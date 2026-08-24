#include "unified_buffer/core/integrity.hpp"
#include <array>

namespace unified_buffer {
namespace {
constexpr std::uint32_t kCrc32cPoly = 0x82F63B78U;  // reversed Castagnoli

const std::array<std::uint32_t, 256>& table() {
  static const auto tbl = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (kCrc32cPoly ^ (c >> 1)) : (c >> 1);
      t[i] = c;
    }
    return t;
  }();
  return tbl;
}
}  // namespace

std::uint32_t crc32c(const void* data, std::size_t len, std::uint32_t seed) {
  const auto& tbl = table();
  std::uint32_t crc = ~seed;
  const auto* p = static_cast<const unsigned char*>(data);
  for (std::size_t i = 0; i < len; ++i) crc = tbl[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

bool crc32c_self_test() {
  // Standard CRC-32C check value for "123456789".
  const char* s = "123456789";
  return crc32c(s, 9) == 0xE3069283U;
}

} // namespace unified_buffer
