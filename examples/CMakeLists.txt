# Example targets.  Every examples/*.cpp becomes a standalone executable
# linked against unified_buffer.  They are NOT registered with CTest (they are
# runnable demos, not assertions).
file(GLOB UB_EXAMPLE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
foreach(src ${UB_EXAMPLE_SOURCES})
  get_filename_component(name ${src} NAME_WE)
  add_executable(${name} ${src})
  target_link_libraries(${name} PRIVATE unified_buffer)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()
