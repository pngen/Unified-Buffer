# ---------------------------------------------------------------------------
# unified_buffer benchmarks.
#
# Every benchmarks/*.cpp is compiled into its own executable that links
# against the unified_buffer static library.  These are timing programs,
# not correctness checks, so they are intentionally NOT registered with
# ctest.
# ---------------------------------------------------------------------------

file(GLOB UNIFIED_BUFFER_BENCH_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

if(UNIFIED_BUFFER_BENCH_SOURCES)
  foreach(BENCH_SRC ${UNIFIED_BUFFER_BENCH_SOURCES})
    get_filename_component(BENCH_NAME ${BENCH_SRC} NAME_WE)
    add_executable(ub_bench_${BENCH_NAME} ${BENCH_SRC})
    target_link_libraries(ub_bench_${BENCH_NAME} PRIVATE UnifiedBuffer::unified_buffer)
    set_target_properties(ub_bench_${BENCH_NAME} PROPERTIES
      OUTPUT_NAME ${BENCH_NAME})
  endforeach()
endif()
