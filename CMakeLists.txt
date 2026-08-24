cmake_minimum_required(VERSION 3.22)
project(unified_buffer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(UNIFIED_BUFFER_ENABLE_CUDA "Enable the CUDA backend when available" ON)
option(UNIFIED_BUFFER_BUILD_TESTS "Build test targets" ON)
option(UNIFIED_BUFFER_BUILD_EXAMPLES "Build example targets" ON)
option(UNIFIED_BUFFER_BUILD_BENCHMARKS "Build benchmark targets" ON)
option(UNIFIED_BUFFER_WARNINGS_AS_ERRORS "Treat project warnings as errors" ON)

if(MSVC)
  add_compile_options(/W4 /utf-8 /permissive- /Zc:__cplusplus)
  if(UNIFIED_BUFFER_WARNINGS_AS_ERRORS)
    add_compile_options(/WX)
  endif()
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
  if(UNIFIED_BUFFER_WARNINGS_AS_ERRORS)
    add_compile_options(-Werror)
  endif()
endif()

# --- CUDA (optional) ---
set(UB_HAS_CUDA OFF)
if(UNIFIED_BUFFER_ENABLE_CUDA)
  find_package(CUDAToolkit QUIET)
  if(CUDAToolkit_FOUND)
    set(UB_HAS_CUDA ON)
  endif()
endif()

file(GLOB_RECURSE UNIFIED_BUFFER_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

add_library(unified_buffer STATIC ${UNIFIED_BUFFER_SOURCES})
add_library(UnifiedBuffer::unified_buffer ALIAS unified_buffer)
target_include_directories(unified_buffer PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_include_directories(unified_buffer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(unified_buffer PUBLIC
  UNIFIED_BUFFER_VERSION_MAJOR=1 UNIFIED_BUFFER_VERSION_MINOR=0 UNIFIED_BUFFER_VERSION_PATCH=0
  UNIFIED_BUFFER_RUNTIME_VERSION="1.0.0")
if(UB_HAS_CUDA)
  target_compile_definitions(unified_buffer PUBLIC UNIFIED_BUFFER_HAS_CUDA=1)
  target_link_libraries(unified_buffer PRIVATE CUDA::cudart)
  target_include_directories(unified_buffer PRIVATE ${CUDAToolkit_INCLUDE_DIRS})
  target_include_directories(unified_buffer PUBLIC ${CUDAToolkit_INCLUDE_DIRS})
endif()

add_executable(unified-buffer cli/main.cpp)
target_link_libraries(unified-buffer PRIVATE unified_buffer)

if(UNIFIED_BUFFER_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(UNIFIED_BUFFER_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(UNIFIED_BUFFER_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

# ---------------------------------------------------------------------------
# Install / package configuration
# ---------------------------------------------------------------------------
include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

install(TARGETS unified_buffer EXPORT UnifiedBufferTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

install(TARGETS unified-buffer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Export the build/install targets.
install(EXPORT UnifiedBufferTargets
  FILE UnifiedBufferTargets.cmake
  NAMESPACE UnifiedBuffer::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/UnifiedBuffer)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/UnifiedBufferConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/UnifiedBufferConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/UnifiedBuffer)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/UnifiedBufferConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/UnifiedBufferConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/UnifiedBufferConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/UnifiedBuffer)