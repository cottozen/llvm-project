set(TSAN_UNIT_TEST_SOURCES
  tsan_dense_alloc_test.cpp
  tsan_flags_test.cpp
  tsan_ilist_test.cpp
  tsan_mman_test.cpp
  tsan_monitor_test.cpp
  tsan_monitor_trace_test.cpp
  tsan_shadow_test.cpp
  tsan_stack_test.cpp
  tsan_sync_test.cpp
  tsan_trace_test.cpp
  tsan_unit_test_main.cpp
  tsan_vector_clock_test.cpp
  )

# Read the repository's Python trace inputs directly at test runtime.
get_filename_component(TSAN_MONITOR_DEFAULT_TRACE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../../../data" ABSOLUTE)
set(TSAN_MONITOR_TRACE_DIR "${TSAN_MONITOR_DEFAULT_TRACE_DIR}" CACHE PATH
  "Directory containing the shared cycles.txt and no_cycles.txt monitor traces")
configure_file(tsan_monitor_trace_config.h.in tsan_monitor_trace_config.h @ONLY)
list(APPEND TSAN_UNITTEST_CFLAGS "-I${CMAKE_CURRENT_BINARY_DIR}")

add_tsan_unittest(TsanUnitTest
  SOURCES ${TSAN_UNIT_TEST_SOURCES}
  HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/../../rtl/tsan_monitor.h
          ${CMAKE_CURRENT_BINARY_DIR}/tsan_monitor_trace_config.h)
