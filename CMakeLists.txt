cmake_minimum_required(VERSION 3.24)

project(dfr
  VERSION 0.1.0
  DESCRIPTION "Deterministic fault injection and recovery for exchange market-data feeds"
  LANGUAGES CXX
)

# Applies to subprojects too, which matters: Catch2 defaults itself to C++14,
# and in C++14 it omits the definition of StringMaker<std::string_view> while
# still declaring it. A C++20 test translation unit then fails to link against
# a symbol that was never emitted. Setting the standard here rather than only
# on our own targets keeps the whole build on one language version.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

# Assertion level is deliberately independent of NDEBUG so that a release build
# can ship with assertions enabled. See docs/DESIGN.md section 2.
#
#   off       no checks; for measuring the cost of the other two
#   fast      bounds checks and frame invariants; the recommended release default
#   paranoid  full pair-assertion density, O(n) invariant walks; simulator default
set(DFR_ASSERTIONS "fast" CACHE STRING "Assertion level: off, fast, or paranoid")
set_property(CACHE DFR_ASSERTIONS PROPERTY STRINGS off fast paranoid)

option(DFR_BUILD_TESTS      "Build the test suite"                      ${PROJECT_IS_TOP_LEVEL})
option(DFR_BUILD_TOOLS      "Build the command-line tools"               ${PROJECT_IS_TOP_LEVEL})
option(DFR_WARNINGS_AS_ERRORS "Treat compiler warnings as errors"        ${PROJECT_IS_TOP_LEVEL})
option(DFR_RECORD_CHOICES   "Compile in the per-draw RNG choice trace"   ON)

set(DFR_SANITIZER "none" CACHE STRING "Sanitizer: none, address, thread, undefined, address+undefined")
set_property(CACHE DFR_SANITIZER PROPERTY STRINGS none address thread undefined address+undefined)

if(NOT DFR_ASSERTIONS MATCHES "^(off|fast|paranoid)$")
  message(FATAL_ERROR "DFR_ASSERTIONS must be off, fast or paranoid (got '${DFR_ASSERTIONS}')")
endif()

# ---------------------------------------------------------------------------
# The library
#
# Header-only for now. When there is enough cold code to be worth compiling
# once, it moves behind a DFR_HEADER_ONLY switch in the fmt/quill style
# (docs/DESIGN.md section 4) without changing this target's name or usage.
# ---------------------------------------------------------------------------

add_library(dfr INTERFACE)
add_library(dfr::dfr ALIAS dfr)

target_compile_features(dfr INTERFACE cxx_std_20)

target_include_directories(dfr INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

string(TOUPPER "${DFR_ASSERTIONS}" _dfr_assert_upper)
target_compile_definitions(dfr INTERFACE
  DFR_ASSERTION_LEVEL=DFR_ASSERTION_LEVEL_${_dfr_assert_upper}
  $<$<BOOL:${DFR_RECORD_CHOICES}>:DFR_RECORD_CHOICES=1>
)

# ---------------------------------------------------------------------------
# Warnings and sanitizers, applied to our own targets only, never exported.
#
# Abseil's headers caution against blanket annotation; the same applies to
# blanket flags. These are development aids, so they live on a private
# interface target that consumers never see.
# ---------------------------------------------------------------------------

add_library(dfr_dev_flags INTERFACE)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(dfr_dev_flags INTERFACE
    -Wall -Wextra -Wpedantic
    -Wconversion -Wsign-conversion   # off-by-one between index, count and size
    -Wshadow
    -Wold-style-cast
    -Wcast-align
    -Wunused
    -Woverloaded-virtual
    -Wnon-virtual-dtor
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough
    -Wextra-semi
    # Determinism: never let the compiler fuse a multiply-add. See
    # BUILD-GUIDE.md section 11.6, leak 11 — Clang defaults to `on`, which
    # emits fmadd and silently changes results between -O levels.
    -ffp-contract=off
  )
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(dfr_dev_flags INTERFACE -Wuseless-cast -Wlogical-op -Wduplicated-cond)
  endif()
  if(DFR_WARNINGS_AS_ERRORS)
    target_compile_options(dfr_dev_flags INTERFACE -Werror)
  endif()
elseif(MSVC)
  target_compile_options(dfr_dev_flags INTERFACE /W4 /permissive- /fp:precise)
  if(DFR_WARNINGS_AS_ERRORS)
    target_compile_options(dfr_dev_flags INTERFACE /WX)
  endif()
endif()

if(NOT DFR_SANITIZER STREQUAL "none")
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    string(REPLACE "+" "," _dfr_san "${DFR_SANITIZER}")
    target_compile_options(dfr_dev_flags INTERFACE
      -fsanitize=${_dfr_san} -fno-omit-frame-pointer -g)
    target_link_options(dfr_dev_flags INTERFACE -fsanitize=${_dfr_san})
  else()
    message(WARNING "DFR_SANITIZER is ignored for ${CMAKE_CXX_COMPILER_ID}")
  endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(DFR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(DFR_BUILD_TOOLS)
  add_subdirectory(tools)

  # Benchmarks travel with the tools rather than behind their own switch: the point of them is the comparison
  # between assertion levels, which needs the same target present in every preset.
  add_subdirectory(bench)
endif()

# ---------------------------------------------------------------------------
# Install / export, so the library is genuinely embeddable rather than
# only buildable in place.
# ---------------------------------------------------------------------------

if(PROJECT_IS_TOP_LEVEL)
  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)

  install(TARGETS dfr EXPORT dfrTargets)
  install(DIRECTORY include/dfr DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

  install(EXPORT dfrTargets
    FILE dfrTargets.cmake
    NAMESPACE dfr::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dfr
  )

  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/dfrConfigVersion.cmake"
    COMPATIBILITY SameMajorVersion
    ARCH_INDEPENDENT
  )
  configure_package_config_file(
    cmake/dfrConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/dfrConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dfr
  )
  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/dfrConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/dfrConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dfr
  )

  message(STATUS "dfr ${PROJECT_VERSION}")
  message(STATUS "  assertions : ${DFR_ASSERTIONS}")
  message(STATUS "  sanitizer  : ${DFR_SANITIZER}")
  message(STATUS "  tests      : ${DFR_BUILD_TESTS}")
  message(STATUS "  tools      : ${DFR_BUILD_TOOLS}")
endif()
