#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace unified_buffer {

// Structured error categories.  Errors carry enough context to debug an
// operation: every failure path emits one of these plus a human-readable
// detail string.  No failure is collapsed to a boolean.
enum class ErrorCode : int16_t {
  ok = 0,
  invalid_argument,
  out_of_capacity,
  quota_exceeded,
  unsupported_domain,
  unsupported_capability,
  stale_handle,
  stale_generation,
  lease_conflict,
  mapping_failure,
  backend_failure,
  device_unavailable,
  invalid_device,
  alignment_error,
  bounds_error,
  integrity_failure,
  import_failure,
  export_failure,
  permission_failure,
  timeout,
  resource_busy,
  lifecycle_violation,
  overflow,
  not_found,
  already_exists,
  closed,
  state_invalid,
  unknown
};

inline std::string_view to_string(ErrorCode c) noexcept {
  switch (c) {
    case ErrorCode::ok: return "ok";
    case ErrorCode::invalid_argument: return "invalid_argument";
    case ErrorCode::out_of_capacity: return "out_of_capacity";
    case ErrorCode::quota_exceeded: return "quota_exceeded";
    case ErrorCode::unsupported_domain: return "unsupported_domain";
    case ErrorCode::unsupported_capability: return "unsupported_capability";
    case ErrorCode::stale_handle: return "stale_handle";
    case ErrorCode::stale_generation: return "stale_generation";
    case ErrorCode::lease_conflict: return "lease_conflict";
    case ErrorCode::mapping_failure: return "mapping_failure";
    case ErrorCode::backend_failure: return "backend_failure";
    case ErrorCode::device_unavailable: return "device_unavailable";
    case ErrorCode::invalid_device: return "invalid_device";
    case ErrorCode::alignment_error: return "alignment_error";
    case ErrorCode::bounds_error: return "bounds_error";
    case ErrorCode::integrity_failure: return "integrity_failure";
    case ErrorCode::import_failure: return "import_failure";
    case ErrorCode::export_failure: return "export_failure";
    case ErrorCode::permission_failure: return "permission_failure";
    case ErrorCode::timeout: return "timeout";
    case ErrorCode::resource_busy: return "resource_busy";
    case ErrorCode::lifecycle_violation: return "lifecycle_violation";
    case ErrorCode::overflow: return "overflow";
    case ErrorCode::not_found: return "not_found";
    case ErrorCode::already_exists: return "already_exists";
    case ErrorCode::closed: return "closed";
    case ErrorCode::state_invalid: return "state_invalid";
    case ErrorCode::unknown: return "unknown";
  }
  return "unknown";
}

// An error carrying an ErrorCode and a diagnostic detail string.
struct Error {
  ErrorCode code = ErrorCode::unknown;
  std::string message;

  Error() = default;
  Error(ErrorCode c, std::string msg = {}) : code(c), message(std::move(msg)) {}

  bool ok() const noexcept { return code == ErrorCode::ok; }
  explicit operator bool() const noexcept { return code != ErrorCode::ok; }
};

} // namespace unified_buffer
