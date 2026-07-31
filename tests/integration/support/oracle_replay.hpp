// Replaying a feed twice: once intact, once through loss and repair.
//
// Split from oracle_feed.hpp, which publishes it. A real seam rather than a line count: a reader checking what the
// venue *sent* never needs the recovery wiring, and a reader checking what the client *delivered* never needs the
// message generator. The vocabulary they share — the feed, a packet, a result — stays in oracle_feed.hpp.

#ifndef DFR_TESTS_INTEGRATION_SUPPORT_ORACLE_REPLAY_HPP
#define DFR_TESTS_INTEGRATION_SUPPORT_ORACLE_REPLAY_HPP

#include "integration/support/oracle_feed.hpp"

namespace dfr_test::oracle {

// The reference: every packet, in order, nothing lost.
inline replay_result replay_clean(const feed& source) {
  replay_result out;
  for (const auto& packet : source.packets) {
    for (std::uint64_t s = packet.first_sequence;
         s < packet.first_sequence + packet.message_count; ++s) {
      const auto found = source.bodies.find(s);
      REQUIRE(found != source.bodies.end());
      detail::apply(out.books,
                    dfr::packet_view{found->second.data(), found->second.size()}, out);
      ++out.delivered;
    }
  }
  return out;
}

// The subject: the same packets through the injector and the recovery client.
//
// Retransmits are answered from the published stream, which is a harness with the whole day in memory rather
// than a facility that forgets — that path is covered in venue_recovery_test.cpp. What is under test here is
// what the *client* delivered, so the server is deliberately not the variable.
inline replay_result replay_recovered(const feed& source, std::uint64_t seed, std::uint32_t faults,
                                      bool glimpse = false,
                                      bool apply_in_arrival_order = false) {
  replay_result out;
  feed_client client{detail::feed_client_options()};
  std::int64_t now = 0;

  chaos::schedule plan;
  if (faults > 0) {
    dfr::prng rng{seed};
    chaos::schedule_options options;
    options.max_faults = faults;
    // The same restriction the sequence oracle uses: the kinds whose consequence a harness can reason about
    // without reimplementing the injector. A bit flip rewrites a body, and this test's whole premise is that
    // recovery does not — so including it would be testing the injector, not the client.
    options.permitted = detail::book_safe_faults();
    REQUIRE(chaos::schedule::generate(rng, options, source.packets.size()).get(plan) ==
            dfr::error::ok);
  }
  chaos::injector<chaos::iextp_target> injector{plan};

  // Delivered sequences, held until they can be applied in order.
  //
  // `next_to_apply` is the sequence the book is waiting for; anything above it waits in `pending` until the hole
  // below it closes. That is the whole of what a consumer of this feed has to do, and it is three lines.
  std::map<std::uint64_t, bool> pending;
  std::uint64_t next_to_apply = 1;

  const auto drain_in_order = [&]() {
    while (pending.erase(next_to_apply) > 0 || source.bodies.count(next_to_apply) == 0) {
      const auto found = source.bodies.find(next_to_apply);
      if (found != source.bodies.end()) {
        detail::apply(out.books, dfr::packet_view{found->second.data(), found->second.size()}, out);
        ++out.delivered;
      } else if (next_to_apply > source.bodies.rbegin()->first) {
        break;  // past the end of what the venue published
      }
      ++next_to_apply;
    }
  };

  const auto deliver = [&](rec::sequence_range range) {
    for (std::uint64_t s = range.first; s < range.end; ++s) {
      if (apply_in_arrival_order) {
        const auto found = source.bodies.find(s);
        if (found == source.bodies.end()) {
          continue;
        }
        detail::apply(out.books, dfr::packet_view{found->second.data(), found->second.size()}, out);
        ++out.delivered;
        continue;
      }
      pending[s] = true;
    }
    if (!apply_in_arrival_order) {
      drain_in_order();
    }
  };

  const auto offer = [&](dfr::packet_view packet) {
    iex::header header;
    if (iex::decode_header(packet).get(header) != dfr::error::ok) {
      return;
    }
    // Framing is checked by walking the messages: a payload whose blocks do not add up fails here exactly as
    // it would in a receiver, and there is no separate validator to call.
    auto walk = iex::message_cursor::over(packet);
    if (!walk) {
      return;
    }
    rec::ingest_report report;
    if (client
            .on_packet(0, header.session, header.first_sequence, header.message_count, 0,
                       at_us(now))
            .get(report) != dfr::error::ok) {
      out.failed = true;
      return;
    }
    if (!report.gap_opened.empty()) {
      ++out.gaps_opened;
    }
    if (report.held_for_replay) {
      for (std::uint64_t s = report.accepted.first; s < report.accepted.end; ++s) {
        const auto found = source.bodies.find(s);
        const char filler = 'm';
        const auto body = found == source.bodies.end()
                              ? dfr::packet_view{&filler, 1}
                              : dfr::packet_view{found->second.data(), found->second.size()};
        if (!client.buffer_message(s, body)) {
          out.failed = true;
          return;
        }
      }
      return;
    }
    // Two independent things, and conflating them was a real bug here: a retransmit packet can repair a hole
    // while delivering nothing new, so `delivered()` is false and the repair still has to reach the book. The
    // first version guarded both with `delivered()` and lost every repair.
    for (const auto& repaired : client.last_recovered().ranges()) {
      deliver(repaired);
    }
    if (report.delivered()) {
      deliver(report.accepted);
    }
  };

  const auto answer = [&]() {
    // Bounded, so a client that asks for the same range forever fails the test rather than hanging it.
    for (int attempt = 0; attempt < 8; ++attempt) {
      const auto decision = client.poll(at_us(now));
      if (decision.what != rec::client_action::send_retransmit_request) {
        return;
      }
      if (glimpse) {
        REQUIRE(client
                    .on_retransmit_refused(decision.range,
                                           dfr::error::retransmit_window_exceeded)
                    .has_value());
        return;
      }
      for (const auto& candidate : source.packets) {
        const rec::sequence_range carried{
            .first = candidate.first_sequence,
            .end = candidate.first_sequence + candidate.message_count};
        if (carried.overlaps(decision.range)) {
          offer(dfr::packet_view{candidate.bytes.data(), candidate.bytes.size()});
        }
      }
    }
  };

  const auto emit = [&](const chaos::emission& e) {
    now += 50;  // past the settle delay, so a hole becomes a request promptly
    offer(e.packet);
    answer();
  };

  for (std::uint64_t i = 0; i < source.packets.size(); ++i) {
    const auto& packet = source.packets[i];
    REQUIRE(injector
                .offer(dfr::packet_view{packet.bytes.data(), packet.bytes.size()}, i, emit)
                .has_value());
  }
  REQUIRE(injector.flush(emit).has_value());
  for (int settle = 0; settle < 32; ++settle) {
    now += 100;
    answer();
  }

  out.unfillable = client.total_missing();
  out.injected = injector.stats().dropped;
  if (client.state() == rec::client_state::failed) {
    out.failed = true;
  }
  return out;
}

}  // namespace dfr_test::oracle

#endif  // DFR_TESTS_INTEGRATION_SUPPORT_ORACLE_REPLAY_HPP
