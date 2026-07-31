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
  # So that a test can include "support/death_test.hpp" regardless of which
  # subdirectory it lives in.
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  catch_discover_tests(${name})
endfunction()

dfr_add_test_suite(dfr_core_tests SOURCES
  core/attributes_test.cpp
  core/assert_test.cpp
  core/error_test.cpp
  core/result_test.cpp
  core/byte_order_test.cpp
  core/packet_view_test.cpp
  core/mutable_packet_view_test.cpp
  core/clock_test.cpp
  core/rng_engine_test.cpp
  core/rng_draws_test.cpp
)

dfr_add_test_suite(dfr_capture_tests SOURCES
  capture/frame_test.cpp
  capture/ethernet_test.cpp
  capture/pcap_test.cpp
  capture/pcapng_timestamp_test.cpp
  capture/pcapng_reader_test.cpp
)

dfr_add_test_suite(dfr_chaos_tests SOURCES
  chaos/fault_test.cpp
  chaos/schedule_test.cpp
  chaos/schedule_generate_test.cpp
  chaos/injector_test.cpp
  chaos/injector_damage_test.cpp
  chaos/oracle_test.cpp
)

dfr_add_test_suite(dfr_recovery_tests SOURCES
  recovery/gap_test.cpp
  recovery/gap_set_test.cpp
  recovery/channel_test.cpp
  recovery/gap_tracker_test.cpp
  recovery/gap_tracker_reset_test.cpp
  recovery/retransmit_policy_test.cpp
  recovery/requester_test.cpp
  recovery/requester_chunking_test.cpp
  recovery/arbiter_test.cpp
  recovery/arbiter_health_test.cpp
  recovery/replay_buffer_test.cpp
  recovery/snapshot_test.cpp
  recovery/client_test.cpp
  recovery/client_recovery_test.cpp
)

dfr_add_test_suite(dfr_trace_tests SOURCES
  trace/recorder_test.cpp
)

dfr_add_test_suite(dfr_venue_tests SOURCES
  venue/publisher_test.cpp
  venue/moldudp64_publisher_test.cpp
  venue/retransmit_facility_test.cpp
  venue/snapshot_facility_test.cpp
  venue/order_entry_test.cpp
  venue/order_replace_test.cpp
  venue/order_session_test.cpp
)

# Spans chaos, wire and recovery on purpose: the properties it checks are exactly the ones
# no single component can be asked about alone.
dfr_add_test_suite(dfr_integration_tests SOURCES
  integration/recovery_oracle_test.cpp
  integration/recovery_repair_test.cpp
  integration/venue_recovery_test.cpp
  integration/venue_snapshot_test.cpp
)

# One suite per protocol, one file per concept within it.
dfr_add_test_suite(dfr_moldudp64_tests SOURCES
  wire/moldudp64_header_test.cpp
  wire/moldudp64_cursor_test.cpp
  wire/moldudp64_encode_test.cpp
)

dfr_add_test_suite(dfr_soupbintcp_tests SOURCES
  wire/soupbintcp_packet_test.cpp
  wire/soupbintcp_ascii_test.cpp
  wire/soupbintcp_session_test.cpp
)

dfr_add_test_suite(dfr_ouch_tests SOURCES
  wire/ouch_value_test.cpp
  wire/ouch_message_test.cpp
)

dfr_add_test_suite(dfr_iextp_tests SOURCES
  wire/iextp_header_test.cpp
  wire/iextp_cursor_test.cpp
  wire/iextp_chain_test.cpp
  wire/iextp_encode_test.cpp
)
