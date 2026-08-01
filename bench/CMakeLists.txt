# Benchmarks, held to the same warning level as everything else.
#
# Not built under a sanitiser, and for two independent reasons rather than to dodge a build error:
#
#   1. `recovery_bench.cpp` replaces the global `operator new` to count allocations, and AddressSanitizer and
#      ThreadSanitizer replace it too. On Linux that is a duplicate-symbol link failure; where it links, the
#      count would be measuring the sanitiser's allocator rather than the library.
#   2. A latency figure taken under a sanitiser is not a latency figure. Instrumented loads and stores move
#      every number by an order of magnitude, so publishing one would be publishing a different quantity.
#
# The properties the benchmark asserts (zero allocations, the ring refusing rather than overwriting) are
# checked by the tests, which *do* run under every sanitiser.
if(DFR_SANITIZER STREQUAL "none")
  find_package(Threads REQUIRED)

  add_executable(dfr_recovery_bench recovery_bench.cpp)
  target_link_libraries(dfr_recovery_bench PRIVATE dfr::dfr dfr_dev_flags)
  target_include_directories(dfr_recovery_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  set_target_properties(dfr_recovery_bench PROPERTIES OUTPUT_NAME recovery_bench)

  # The only benchmark that uses two cores. Separate from recovery_bench because it measures a different thing
  # and because a caller profiling the single-threaded path should not have to start threads to do it.
  add_executable(dfr_handoff_bench handoff_bench.cpp)
  target_link_libraries(dfr_handoff_bench PRIVATE dfr::dfr dfr_dev_flags Threads::Threads)
  target_include_directories(dfr_handoff_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  set_target_properties(dfr_handoff_bench PROPERTIES OUTPUT_NAME handoff_bench)
else()
  message(STATUS "dfr: benchmarks skipped under DFR_SANITIZER=${DFR_SANITIZER}")
endif()
