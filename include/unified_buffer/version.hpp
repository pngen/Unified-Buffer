#pragma once
#define UNIFIED_BUFFER_VERSION_MAJOR 1
#define UNIFIED_BUFFER_VERSION_MINOR 0
#define UNIFIED_BUFFER_VERSION_PATCH 0
#define UNIFIED_BUFFER_VERSION_STRING "1.0.0"

namespace unified_buffer {
// Returns the runtime version string (e.g. "1.0.0").
const char* version_string() noexcept;
}
