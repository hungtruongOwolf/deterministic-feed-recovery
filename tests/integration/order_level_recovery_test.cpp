// Order identity through the complete recovery path.
//
// The DEEP oracle proves an aggregated price-level book survives loss. This proves the stronger order-level
// statement: Add, Execute, Cancel, Replace and Delete messages cross MoldUDP64, deterministic packet damage,
// retransmission and the ordered thread-boundary publisher, and the final live-order set equals the one that
// never lost a packet. Recovery remains unaware of ITCH; it sees only session and sequence ranges.

#include <dfr/book/order_level_book.hpp>
#include <dfr/chaos/injector.hpp>
#include <dfr/concurrent/publisher.hpp>
#include <dfr/recovery/client.hpp>
#include <dfr/venue/publisher.hpp>
#include <dfr/venue/retransmit_facility.hpp>
#include <dfr/wire/itch.hpp>
#include <dfr/wire/moldudp64.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace {

namespace chaos = dfr::chaos;
namespace conc = dfr::concurrent;
namespace itch = dfr::wire::itch;
namespace mold = dfr::wire::moldudp64;
namespace rec = dfr::recovery;
namespace ven = dfr::venue;

using clock_type = dfr::manual_clock;
using order_book = dfr::book::order_level_book<2048>;
using feed_publisher = ven::moldudp64_publisher<clock_type>;
using recovery_client = rec::client<clock_type, rec::replay_buffer<16 * 1024, 1024>>;
using ordered_publisher = conc::publisher<4096, 256>;

struct packet {
  std::string bytes;
  std::uint64_t first_sequence{0};
  std::uint64_t message_count{0};
};

struct feed {
  std::vector<packet> packets;
  std::vector<std::string> bodies;
};

[[nodiscard]] rec::client_options client_options() {
  rec::client_options options;
  options.lines = 1;
  options.arbitration.detect_divergence = false;
  options.retransmission.settle_delay = dfr::duration::zero();
  options.retransmission.first_timeout = std::chrono::microseconds{10};
  options.retransmission.max_timeout = std::chrono::microseconds{40};
  options.retransmission.max_attempts = 3;
  options.retransmission.retention_window = std::chrono::seconds{2};
  REQUIRE(options.validate().has_value());
  return options;
}

[[nodiscard]] dfr::result<void> apply(order_book& book, dfr::packet_view body) {
  itch::header head;
  if (const auto err = itch::decode_header(body).get(head); err != dfr::error::ok) {
    return err;
  }
  switch (head.type) {
    case itch::message_type::add_order: {
      itch::add_order message;
      if (const auto err = itch::decode_add_order(body).get(message); err != dfr::error::ok) {
        return err;
      }
      return book.apply(message);
    }
    case itch::message_type::order_executed: {
      itch::order_executed message;
      if (const auto err = itch::decode_order_executed(body).get(message); err != dfr::error::ok) {
        return err;
      }
      return book.apply(message);
    }
    case itch::message_type::order_cancel: {
      itch::order_cancel message;
      if (const auto err = itch::decode_order_cancel(body).get(message); err != dfr::error::ok) {
        return err;
      }
      return book.apply(message);
    }
    case itch::message_type::order_delete: {
      itch::order_delete message;
      if (const auto err = itch::decode_order_delete(body).get(message); err != dfr::error::ok) {
        return err;
      }
      return book.apply(message);
    }
    case itch::message_type::order_replace: {
      itch::order_replace message;
      if (const auto err = itch::decode_order_replace(body).get(message); err != dfr::error::ok) {
        return err;
      }
      return book.apply(message);
    }
  }
  DFR_UNREACHABLE("unhandled ITCH message type");
}

template <typename Encode>
[[nodiscard]] std::string encoded(Encode&& encode) {
  std::array<std::byte, 64> bytes{};
  std::size_t size = 0;
  REQUIRE(encode(dfr::mutable_packet_view{bytes.data(), bytes.size()}).get(size) ==
          dfr::error::ok);
  return std::string{reinterpret_cast<const char*>(bytes.data()), size};
}

[[nodiscard]] feed publish_order_feed() {
  feed out;
  feed_publisher publisher{ven::publisher_options{
      .session = 42, .channel = 1, .first_sequence = 1,
      .first_stream_offset = 0, .heartbeat_interval = std::chrono::seconds{1}}};
  clock_type clock;

  const auto capture = [&](dfr::packet_view datagram) {
    mold::header header;
    REQUIRE(mold::decode_header(datagram).get(header) == dfr::error::ok);
    out.packets.push_back(packet{
        .bytes = std::string{reinterpret_cast<const char*>(datagram.data()), datagram.size()},
        .first_sequence = header.sequence,
        .message_count = header.message_count});
  };

  std::uint16_t tracking = 1;
  const auto submit = [&](std::string body) {
    out.bodies.push_back(body);
    REQUIRE(publisher.submit(dfr::packet_view{out.bodies.back().data(), out.bodies.back().size()},
                             clock.now(), capture)
                .has_value());
    if (out.bodies.size() % 4 == 0) {
      REQUIRE(publisher.flush(clock.now(), capture).has_value());
    }
  };

  for (std::uint64_t group = 0; group < 80; ++group) {
    const std::uint64_t first = group * 10 + 1;
    const itch::header_fields fields{
        .timestamp_ns = 1'000 + group, .stock_locate = 7, .tracking_number = tracking++};
    submit(encoded([&](dfr::mutable_packet_view out_bytes) {
      return itch::encode_add_order(out_bytes, fields, first, 'B', 100, "IEXT",
                                    208'000 + static_cast<std::uint32_t>(group));
    }));
    submit(encoded([&](dfr::mutable_packet_view out_bytes) {
      return itch::encode_add_order(out_bytes, fields, first + 1, 'S', 90, "IEXT",
                                    210'000 + static_cast<std::uint32_t>(group));
    }));
    submit(encoded([&](dfr::mutable_packet_view out_bytes) {
      return itch::encode_order_executed(out_bytes, fields, first, 20, 50'000 + group);
    }));
    submit(encoded([&](dfr::mutable_packet_view out_bytes) {
      return itch::encode_order_cancel(out_bytes, fields, first + 1, 10);
    }));
    submit(encoded([&](dfr::mutable_packet_view out_bytes) {
      return itch::encode_order_replace(out_bytes, fields, first, first + 2, 70,
                                        209'000 + static_cast<std::uint32_t>(group));
    }));
    submit(encoded([&](dfr::mutable_packet_view out_bytes) {
      return itch::encode_order_delete(out_bytes, fields, first + 1);
    }));
    submit(encoded([&](dfr::mutable_packet_view out_bytes) {
      return itch::encode_add_order(out_bytes, fields, first + 3, group % 2 == 0 ? 'B' : 'S',
                                    40, "IEXT", 207'000 + static_cast<std::uint32_t>(group));
    }));
  }
  REQUIRE(publisher.flush(clock.now(), capture).has_value());
  REQUIRE(publisher.stats().messages == out.bodies.size());
  return out;
}

[[nodiscard]] chaos::schedule damage_plan() {
  chaos::schedule plan;
  REQUIRE(plan.add(chaos::fault{.op = chaos::fault_op::drop,
                                .first_packet = 5,
                                .packet_count = 2})
              .has_value());
  REQUIRE(plan.add(chaos::fault{.op = chaos::fault_op::delay,
                                .first_packet = 31,
                                .packet_count = 1,
                                .parameter = 4})
              .has_value());
  REQUIRE(plan.add(chaos::fault{.op = chaos::fault_op::duplicate,
                                .first_packet = 70,
                                .packet_count = 1})
              .has_value());
  REQUIRE(plan.add(chaos::fault{.op = chaos::fault_op::drop,
                                .first_packet = 101,
                                .packet_count = 1})
              .has_value());
  return plan;
}

[[nodiscard]] bool contains(const rec::gap_set& ranges, std::uint64_t sequence) {
  for (const rec::sequence_range range : ranges.ranges()) {
    if (range.contains(sequence)) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST_CASE("an order-level book survives MoldUDP64 loss and repair",
          "[integration][order-level]") {
  const feed source = publish_order_feed();
  REQUIRE(source.packets.size() > 120);

  order_book reference;
  for (const std::string& body : source.bodies) {
    REQUIRE(apply(reference, dfr::packet_view{body.data(), body.size()}).has_value());
  }
  REQUIRE(reference.size() == 160);

  ven::retransmit_facility<256> facility;
  for (const packet& datagram : source.packets) {
    REQUIRE(facility.record(datagram.first_sequence, datagram.message_count,
                            dfr::packet_view{datagram.bytes.data(), datagram.bytes.size()})
                .has_value());
  }

  recovery_client client{client_options()};
  ordered_publisher ordered{1};
  order_book recovered;
  chaos::injector<chaos::moldudp64_target> injector{damage_plan()};
  auto now = clock_type::time_point{};
  std::uint64_t consumed = 0;
  std::uint64_t repaired = 0;

  const auto drain = [&]() {
    std::array<conc::delivery, 64> batch{};
    while (const std::size_t count = ordered.ring().pop_batch(batch.data(), batch.size())) {
      for (std::size_t i = 0; i < count; ++i) {
        REQUIRE(apply(recovered, batch[i].payload()).has_value());
        repaired += batch[i].recovered ? 1 : 0;
        ++consumed;
      }
    }
  };

  const auto offer = [&](dfr::packet_view datagram) {
    mold::message_cursor cursor;
    if (mold::message_cursor::over(datagram).get(cursor) != dfr::error::ok) {
      return;
    }
    const mold::header header = cursor.packet_header();
    std::array<mold::message, 64> messages{};
    std::size_t count = 0;
    while (!cursor.done()) {
      REQUIRE(count < messages.size());
      REQUIRE(cursor.next().get(messages[count]) == dfr::error::ok);
      ++count;
    }
    REQUIRE(cursor.rest().empty());

    rec::ingest_report report;
    if (client.on_packet(0, 42, header.sequence, header.message_count, 0, now).get(report) !=
        dfr::error::ok) {
      return;
    }
    for (const mold::message& message : std::span{messages}.first(count)) {
      const bool is_repaired = contains(client.last_recovered(), message.sequence);
      const bool is_accepted = report.accepted.contains(message.sequence);
      if (is_repaired || is_accepted) {
        REQUIRE(ordered.offer(message.sequence, 0, is_repaired, message.payload));
      }
    }
    drain();
  };

  const auto answer = [&]() {
    for (std::size_t step = 0; step < 64; ++step) {
      const rec::client_decision decision = client.poll(now);
      if (decision.what != rec::client_action::send_retransmit_request) {
        break;
      }
      const auto served = facility.serve(decision.range, offer);
      REQUIRE(served.has_value());
    }
  };

  const auto emit = [&](const chaos::emission& emission) {
    now += std::chrono::microseconds{20};
    offer(emission.packet);
    answer();
  };
  for (std::size_t i = 0; i < source.packets.size(); ++i) {
    const packet& datagram = source.packets[i];
    REQUIRE(injector.offer(dfr::packet_view{datagram.bytes.data(), datagram.bytes.size()}, i, emit)
                .has_value());
  }
  REQUIRE(injector.flush(emit).has_value());
  answer();
  drain();

  CHECK(client.state() == rec::client_state::live);
  CHECK(client.total_missing() == 0);
  CHECK(client.retransmission().stats().requests_sent > 0);
  CHECK(facility.stats().messages_served > 0);
  CHECK(injector.stats().dropped == 3);
  CHECK(injector.stats().delayed == 1);
  CHECK(injector.stats().duplicated == 1);
  CHECK(ordered.stats().reordered > 0);
  CHECK(ordered.stats().refused == 0);
  CHECK(repaired > 0);
  CHECK(consumed == source.bodies.size());
  CHECK(recovered == reference);
}
