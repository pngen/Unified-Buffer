#pragma once
#include <cstdint>
#include <string_view>
#include <vector>

namespace unified_buffer {

// Capability flags advertised by a memory domain / backend.  Capabilities are
// queried programmatically; unsupported operations are rejected before side
// effects are performed.
enum class Capability : uint16_t {
  HOST_READABLE = 0,
  HOST_WRITABLE,
  DEVICE_READABLE,
  DEVICE_WRITABLE,
  CPU_MAPPABLE,
  DEVICE_MAPPABLE,
  EXPORTABLE,
  IMPORTABLE,
  SHAREABLE_ACROSS_PROCESS,
  SHAREABLE_ACROSS_DEVICE,
  PERSISTENT,
  PINNED,
  COHERENT,
  ASYNC_COPYABLE,
  PAGEABLE,
  DIRECTLY_DMA_CAPABLE,
  SUPPORTS_SUBALLOCATION,
  SUPPORTS_EXTERNAL_HANDLE,
  SUPPORTS_ZERO_COPY,
  SUPPORTS_EXPLICIT_FLUSH,
  SUPPORTS_EXPLICIT_INVALIDATE,
  COUNT,
};

inline std::string_view to_string(Capability c) noexcept {
  switch (c) {
    case Capability::HOST_READABLE: return "HOST_READABLE";
    case Capability::HOST_WRITABLE: return "HOST_WRITABLE";
    case Capability::DEVICE_READABLE: return "DEVICE_READABLE";
    case Capability::DEVICE_WRITABLE: return "DEVICE_WRITABLE";
    case Capability::CPU_MAPPABLE: return "CPU_MAPPABLE";
    case Capability::DEVICE_MAPPABLE: return "DEVICE_MAPPABLE";
    case Capability::EXPORTABLE: return "EXPORTABLE";
    case Capability::IMPORTABLE: return "IMPORTABLE";
    case Capability::SHAREABLE_ACROSS_PROCESS: return "SHAREABLE_ACROSS_PROCESS";
    case Capability::SHAREABLE_ACROSS_DEVICE: return "SHAREABLE_ACROSS_DEVICE";
    case Capability::PERSISTENT: return "PERSISTENT";
    case Capability::PINNED: return "PINNED";
    case Capability::COHERENT: return "COHERENT";
    case Capability::ASYNC_COPYABLE: return "ASYNC_COPYABLE";
    case Capability::PAGEABLE: return "PAGEABLE";
    case Capability::DIRECTLY_DMA_CAPABLE: return "DIRECTLY_DMA_CAPABLE";
    case Capability::SUPPORTS_SUBALLOCATION: return "SUPPORTS_SUBALLOCATION";
    case Capability::SUPPORTS_EXTERNAL_HANDLE: return "SUPPORTS_EXTERNAL_HANDLE";
    case Capability::SUPPORTS_ZERO_COPY: return "SUPPORTS_ZERO_COPY";
    case Capability::SUPPORTS_EXPLICIT_FLUSH: return "SUPPORTS_EXPLICIT_FLUSH";
    case Capability::SUPPORTS_EXPLICIT_INVALIDATE: return "SUPPORTS_EXPLICIT_INVALIDATE";
    case Capability::COUNT: return "COUNT";
  }
  return "UNKNOWN";
}

// Compact capability set: one bit per Capability.
class Capabilities {
 public:
  Capabilities() = default;
  void set(Capability c) { mask_ |= (uint64_t(1) << static_cast<uint16_t>(c)); }
  void clear(Capability c) { mask_ &= ~(uint64_t(1) << static_cast<uint16_t>(c)); }
  [[nodiscard]] bool has(Capability c) const { return (mask_ & (uint64_t(1) << static_cast<uint16_t>(c))) != 0; }
  [[nodiscard]] bool has_all(std::initializer_list<Capability> cs) const {
    for (Capability c : cs) if (!has(c)) return false;
    return true;
  }
  [[nodiscard]] uint64_t mask() const { return mask_; }
  std::vector<Capability> enabled() const {
    std::vector<Capability> out;
    for (uint16_t i = 0; i < static_cast<uint16_t>(Capability::COUNT); ++i)
      if (mask_ & (uint64_t(1) << i)) out.push_back(static_cast<Capability>(i));
    return out;
  }
 private:
  uint64_t mask_ = 0;
};

} // namespace unified_buffer
