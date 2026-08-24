#include "internal.hpp"
#include <cstring>
#ifdef UNIFIED_BUFFER_HAS_CUDA
#include <cuda_runtime.h>
#endif

namespace unified_buffer {
namespace internal {

Status copy_memory(void* dst, const void* src, std::uint64_t len, bool dst_is_device, bool src_is_device) {
  if (len == 0) return ok_status();
#ifdef UNIFIED_BUFFER_HAS_CUDA
  if (dst_is_device || src_is_device) {
    cudaMemcpyKind kind;
    if (dst_is_device && src_is_device) kind = cudaMemcpyDeviceToDevice;
    else if (dst_is_device && !src_is_device) kind = cudaMemcpyHostToDevice;
    else kind = cudaMemcpyDeviceToHost;
    cudaError_t e = cudaMemcpy(dst, src, static_cast<std::size_t>(len), kind);
    if (e != cudaSuccess) return Error(ErrorCode::backend_failure, std::string("cudaMemcpy: ") + cudaGetErrorString(e));
    if (cudaDeviceSynchronize() != cudaSuccess) return Error(ErrorCode::backend_failure, "copy sync failed");
    return ok_status();
  }
#else
  (void)dst_is_device; (void)src_is_device;
#endif
  std::memcpy(dst, src, static_cast<std::size_t>(len));
  return ok_status();
}

}  // namespace internal
}
