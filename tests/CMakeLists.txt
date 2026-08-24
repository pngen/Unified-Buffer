# Test targets.  Every test_*.cpp in this directory becomes a small executable
# registered with CTest.  Each test returns the number of failed checks as its
# exit code (0 = pass).
file(GLOB UB_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)
foreach(src ${UB_TEST_SOURCES})
  get_filename_component(name ${src} NAME_WE)
  add_executable(${name} ${src})
  target_link_libraries(${name} PRIVATE unified_buffer)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME ${name} COMMAND ${name})
endforeach()
