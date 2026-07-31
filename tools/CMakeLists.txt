# Tools are held to the same warning level as the library, so a tool cannot become
# the place where sloppy code accumulates.
add_executable(dfr_inspect inspect.cpp)
target_link_libraries(dfr_inspect PRIVATE dfr::dfr dfr_dev_flags)
set_target_properties(dfr_inspect PROPERTIES OUTPUT_NAME inspect)

# Runs the full pipeline — inject, decode, recover — over a real capture and checks it
# accounts for itself. The unit oracle in tests/integration does the same over synthetic
# packets; this answers whether it holds on a day of actual exchange traffic.
add_executable(dfr_verify verify.cpp)
target_link_libraries(dfr_verify PRIVATE dfr::dfr dfr_dev_flags)
target_include_directories(dfr_verify PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(dfr_verify PROPERTIES OUTPUT_NAME verify)
