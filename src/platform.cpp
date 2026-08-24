#include "internal.hpp"
#include "unified_buffer/backends/shared_memory_backend.hpp"
#include "unified_buffer/backends/file_backend.hpp"

namespace unified_buffer {
namespace internal {

void RuntimeImpl::attach_shared_and_file_backends() {
  if (cfg.enable_shared) shared_ = std::make_unique<SharedMemoryDomain>(cfg.shared_cap);
  if (cfg.enable_file) file_ = std::make_unique<FileBackedDomain>(cfg.file_cap);
}

}  // namespace internal
}
