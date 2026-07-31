// Shared setup for the recovery client tests.

#ifndef DFR_TESTS_RECOVERY_SUPPORT_CLIENT_FIXTURE_HPP
#define DFR_TESTS_RECOVERY_SUPPORT_CLIENT_FIXTURE_HPP

#include <dfr/recovery/client.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <string_view>

namespace dfr_test::recovery {

namespace rec = dfr::recovery;

// A small replay buffer, so the overflow path is reachable in a test rather than
// theoretical.
using test_client = rec::client<dfr::manual_clock, rec::replay_buffer<128, 8>>;
using test_time = dfr::manual_clock::time_point;

inline constexpr std::uint32_t kSession = 0xABCD;
inline constexpr std::size_t kLineA = 0;
inline constexpr std::size_t kLineB = 1;

constexpr rec::sequence_range range(std::uint64_t first, std::uint64_t end) {
  return rec::sequence_range{.first = first, .end = end};
}

inline test_time at_ms(std::int64_t millis) {
  return test_time{} + std::chrono::milliseconds{millis};
}

// Round numbers, so a test reads as a timeline: settle 1ms, then 10ms and 20ms between
// attempts, three attempts, one second of retention.
inline rec::client_options readable_options() {
  rec::client_options options;
  options.lines = 2;
  options.arbitration.liveness_timeout = std::chrono::milliseconds{100};
  options.retransmission.settle_delay = std::chrono::milliseconds{1};
  options.retransmission.first_timeout = std::chrono::milliseconds{10};
  options.retransmission.max_timeout = std::chrono::milliseconds{500};
  options.retransmission.max_attempts = 3;
  options.retransmission.retention_window = std::chrono::seconds{1};
  REQUIRE(options.validate().has_value());
  return options;
}

// Offers a packet and requires it to be accepted, returning what happened. A failure is a
// bug in the test everywhere except the tests that are about failure.
inline rec::ingest_report offer(test_client& client, std::size_t line,
                                rec::sequence_range arrived, test_time now,
                                std::uint32_t session = kSession,
                                std::uint64_t digest = 0) {
  rec::ingest_report out;
  REQUIRE(client
              .on_packet(line, session, arrived.first, arrived.count(), digest, now)
              .get(out) == dfr::error::ok);
  return out;
}

inline dfr::packet_view bytes_of(std::string_view text) {
  return dfr::packet_view{text.data(), text.size()};
}

// Hands every message of a held range to the client, as the caller is expected to.
inline void hand_over(test_client& client, rec::sequence_range held,
                      std::string_view payload = "m") {
  for (std::uint64_t sequence = held.first; sequence < held.end; ++sequence) {
    REQUIRE(client.buffer_message(sequence, bytes_of(payload)).has_value());
  }
}

}  // namespace dfr_test::recovery

#endif  // DFR_TESTS_RECOVERY_SUPPORT_CLIENT_FIXTURE_HPP
