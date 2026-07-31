# Fuzz targets, and two ways to drive them.
#
# Each target is a plain `LLVMFuzzerTestOneInput`. Where the toolchain ships libFuzzer we link it and get
# coverage-guided exploration; where it does not — the Xcode clang this project is developed on, for one — the
# portable driver in driver.cpp runs the same target from a seed. Both matter: the first finds more, the second
# means the person writing a decoder can fuzz it without a CI round trip, and reproduces from two numbers.
#
# Sanitisers are not optional here. A fuzzer without them checks that a decoder does not segfault, which is the
# least interesting of its three jobs.

include(CheckCXXSourceCompiles)

set(DFR_FUZZ_TARGETS iextp moldudp64 soupbintcp ouch deep capture)

set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
check_cxx_source_compiles(
  "extern \"C\" int LLVMFuzzerTestOneInput(const unsigned char*, unsigned long) { return 0; }"
  DFR_HAVE_LIBFUZZER)
unset(CMAKE_REQUIRED_FLAGS)

foreach(target IN LISTS DFR_FUZZ_TARGETS)
  add_executable(dfr_fuzz_${target} targets/${target}.cpp)
  target_link_libraries(dfr_fuzz_${target} PRIVATE dfr::dfr dfr_dev_flags)
  target_include_directories(dfr_fuzz_${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  set_target_properties(dfr_fuzz_${target} PROPERTIES OUTPUT_NAME fuzz_${target})

  if(DFR_HAVE_LIBFUZZER)
    target_compile_options(dfr_fuzz_${target} PRIVATE -fsanitize=fuzzer,address,undefined
                                                      -fno-omit-frame-pointer)
    target_link_options(dfr_fuzz_${target} PRIVATE -fsanitize=fuzzer,address,undefined)
  else()
    target_sources(dfr_fuzz_${target} PRIVATE driver.cpp)
    target_compile_options(dfr_fuzz_${target} PRIVATE -fsanitize=address,undefined
                                                      -fno-omit-frame-pointer)
    target_link_options(dfr_fuzz_${target} PRIVATE -fsanitize=address,undefined)
  endif()
endforeach()

if(DFR_HAVE_LIBFUZZER)
  message(STATUS "dfr: fuzzing with libFuzzer")
else()
  message(STATUS "dfr: libFuzzer not available; using the portable seeded driver")
endif()
