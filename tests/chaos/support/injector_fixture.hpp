// Shared setup for the injector tests.
//
// Two test files drive one injector — delivery faults and damage faults — and
// they need the same stream and the same collector. Here rather than copied, per
// docs/STYLE.md §1.10: shared fixtures live in a support/ header.
//
// Every free function is `inline`: without it, three translation units including
// this header produce duplicate symbols at link time.

#ifndef DFR_TESTS_CHAOS_SUPPORT_INJECTOR_FIXTURE_HPP
#define DFR_TESTS_CHAOS_SUPPORT_INJECTOR_FIXTURE_HPP

#include <dfr/chaos/injector.hpp>
#include <dfr/chaos/target.hpp>

#include "wire/support/raw_iextp.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dfr_test::chaos {

namespace chaos = dfr::chaos;

using iex_injector = chaos::injector<chaos::iextp_target>;

// One emitted packet, copied so it survives the injector's scratch buffer. The
// copy is the test proving it understood the lifetime contract.
struct captured {
  std::string bytes;
  chaos::fault_op cause{chaos::fault_op::none};
  std::uint64_t source_index{0};
  bool is_duplicate{false};
};

inline std::vector<captured> collect(iex_injector& into,
                             const std::vector<std::string>& stream) {
  std::vector<captured> out;
  const auto emit = [&](const chaos::emission& e) {
    out.push_back(captured{
        .bytes = std::string{reinterpret_cast<const char*>(e.packet.data()),
                             e.packet.size()},
        .cause = e.cause,
        .source_index = e.source_index,
        .is_duplicate = e.is_duplicate});
  };

  for (std::uint64_t i = 0; i < stream.size(); ++i) {
    const dfr::packet_view view{stream[i].data(), stream[i].size()};
    REQUIRE(into.offer(view, i, emit).has_value());
  }
  REQUIRE(into.flush(emit).has_value());
  return out;
}

// A stream of well-formed IEX-TP packets whose payload identifies its index, so
// an emitted packet can be traced back to its source.
inline std::vector<std::string> iextp_stream(std::size_t count) {
  std::vector<std::string> out;
  out.reserve(count);
  std::uint64_t sequence = 1;
  std::int64_t offset = 0;

  for (std::size_t i = 0; i < count; ++i) {
    dfr_test::iex::raw_packet packet;
    const std::string payload = "p" + std::to_string(i);
    packet.session(0xABCD)
        .channel(1)
        .first_sequence(sequence)
        .stream_offset(offset)
        .count(1)
        .block(payload)
        .seal();
    const auto view = packet.view();
    out.emplace_back(reinterpret_cast<const char*>(view.data()), view.size());

    sequence += 1;
    offset += static_cast<std::int64_t>(2 + payload.size());
  }
  return out;
}

// The same stream with two blocks per packet, for the faults that need a count
// there is room to lower without reaching zero.
inline std::vector<std::string> iextp_stream_of_two_blocks(std::size_t count) {
  std::vector<std::string> out;
  out.reserve(count);
  std::uint64_t sequence = 1;
  std::int64_t offset = 0;

  for (std::size_t i = 0; i < count; ++i) {
    dfr_test::iex::raw_packet packet;
    const std::string first = "a" + std::to_string(i);
    const std::string second = "b" + std::to_string(i);
    packet.session(0xABCD)
        .channel(1)
        .first_sequence(sequence)
        .stream_offset(offset)
        .count(2)
        .block(first)
        .block(second)
        .seal();
    const auto view = packet.view();
    out.emplace_back(reinterpret_cast<const char*>(view.data()), view.size());

    sequence += 2;
    offset += static_cast<std::int64_t>(4 + first.size() + second.size());
  }
  return out;
}

inline chaos::schedule one_fault(chaos::fault entry) {
  chaos::schedule s;
  REQUIRE(s.add(entry).has_value());
  return s;
}

}  // namespace dfr_test::chaos

#endif  // DFR_TESTS_CHAOS_SUPPORT_INJECTOR_FIXTURE_HPP
