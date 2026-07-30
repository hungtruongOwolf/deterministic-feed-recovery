# Tools are held to the same warning level as the library, so a tool cannot become
# the place where sloppy code accumulates.
add_executable(dfr_inspect inspect.cpp)
target_link_libraries(dfr_inspect PRIVATE dfr::dfr dfr_dev_flags)
set_target_properties(dfr_inspect PROPERTIES OUTPUT_NAME inspect)
