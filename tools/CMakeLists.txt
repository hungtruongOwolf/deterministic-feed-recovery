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

# Records one complete run of the library as JSONL, for the viewer to read. A trace is a
# deterministic function of the seed, so it can be committed next to the test that produced it.
add_executable(dfr_trace trace.cpp)
target_link_libraries(dfr_trace PRIVATE dfr::dfr dfr_dev_flags)
target_include_directories(dfr_trace PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(dfr_trace PROPERTIES OUTPUT_NAME trace)

# Drives one order-entry session and prints both directions of the wire, with a client cursor counting
# the server's stream alongside it. The seam between SoupBinTCP and OUCH is the kind of thing that
# benefits from being watched rather than only asserted.
add_executable(dfr_session session.cpp)
target_link_libraries(dfr_session PRIVATE dfr::dfr dfr_dev_flags)
target_include_directories(dfr_session PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(dfr_session PROPERTIES OUTPUT_NAME session)
