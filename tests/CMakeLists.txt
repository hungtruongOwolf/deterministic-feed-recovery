include(FetchContent)

# Catch2 is a test-only dependency and never appears in the installed package.
# Pinned to a tag rather than a branch so that a failing test is reproducible
# from a commit alone.
FetchContent_Declare(Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.7.1
  GIT_SHALLOW    TRUE
  SYSTEM                    # its headers must not be held to our warning level
  EXCLUDE_FROM_ALL
  FIND_PACKAGE_ARGS 3 NAMES Catch2
)
FetchContent_MakeAvailable(Catch2)

list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
include(Catch)

# ---------------------------------------------------------------------------
# One test binary per component, so a failure names the component in CI output
# and so a slow component can be run alone.
# ---------------------------------------------------------------------------

function(dfr_add_test_suite name)
  cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
  add_executable(${name} ${ARG_SOURCES})
  target_link_libraries(${name} PRIVATE dfr::dfr dfr_dev_flags Catch2::Catch2WithMain)
  catch_discover_tests(${name})
endfunction()

dfr_add_test_suite(dfr_core_tests SOURCES
  core/attributes_test.cpp
  core/assert_test.cpp
  core/error_test.cpp
  core/result_test.cpp
  core/byte_order_test.cpp
  core/packet_view_test.cpp
)
