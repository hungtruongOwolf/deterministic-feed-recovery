# Benchmarks, held to the same warning level as everything else.
#
# Built for every preset rather than only for `bench`, on purpose: the interesting comparison is the same
# binary with assertions at paranoid and with them off, and a target that only existed in one configuration
# could not make it.
add_executable(dfr_recovery_bench recovery_bench.cpp)
target_link_libraries(dfr_recovery_bench PRIVATE dfr::dfr dfr_dev_flags)
target_include_directories(dfr_recovery_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(dfr_recovery_bench PROPERTIES OUTPUT_NAME recovery_bench)
