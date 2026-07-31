// The seam between a SoupBinTCP stream and an order-entry host.
//
// Both halves were tested already and both were correct. These tests are about the joins, because a join
// is where a defect lives without either side being wrong: who numbers an acknowledgement and when, what
// a server does with a packet only a server may send, what becomes of an order that arrives before its
// login. None of those questions can be put to either component alone, which is why they went unasked
// until the component that has to answer them existed.

#include <dfr/venue/order_session.hpp>

#include "venue/support/order_session_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

using namespace dfr_test::order_session;  // NOLINT(google-build-using-namespace)

TEST_CASE("a login is answered, and the answer names where the server is") {
  auto host = fresh_session();
  client_stream stream;
  wire_log out;

  CHECK(host.phase() == venue::session_phase::awaiting_login);
  const auto accepted = log_in(host, stream, out, at_ms(0));

  REQUIRE(out.count() == 1);
  CHECK(out.at(0).type == soup::packet_type::login_accepted);
  CHECK(accepted.session == "DFRSESSION");
  // The number is the sequence of the *next* Sequenced Data Packet. Had the session numbered the login
  // itself this would be 2, and every acknowledgement would be off by one for the life of the
  // connection — with both halves still passing their own tests.
  CHECK(accepted.next_sequence == 1);
  CHECK(host.next_sequence() == 1);
}

TEST_CASE("wrong credentials are rejected, and the session does not open") {
  auto host = fresh_session();
  client_stream stream;
  wire_log out;

  stream.put_login("DFRUSR", "wrong");
  std::size_t taken = 0;
  REQUIRE(host.offer(stream.view(), at_ms(0), out).get(taken) == dfr::error::ok);

  REQUIRE(out.count() == 1);
  CHECK(out.at(0).type == soup::packet_type::login_rejected);
  soup::reject_reason reason{};
  REQUIRE(soup::decode_login_rejected(out.at(0).payload).get(reason) == dfr::error::ok);
  CHECK(reason == soup::reject_reason::not_authorized);
  CHECK(host.phase() == venue::session_phase::ended);
  CHECK(host.stats().logins_rejected == 1);
}

TEST_CASE("a login naming a different day is refused for that reason, not for the other one") {
  auto host = fresh_session();
  client_stream stream;
  wire_log out;

  stream.put_login("DFRUSR", "DFRPASS", "OTHERDAY0");
  std::size_t taken = 0;
  REQUIRE(host.offer(stream.view(), at_ms(0), out).get(taken) == dfr::error::ok);

  REQUIRE(out.count() == 1);
  soup::reject_reason reason{};
  REQUIRE(soup::decode_login_rejected(out.at(0).payload).get(reason) == dfr::error::ok);
  // The credentials were fine and the day was not. A client retrying tomorrow has to be able to tell
  // those apart, which is the whole reason the protocol has two reject codes.
  CHECK(reason == soup::reject_reason::session_not_available);
}

TEST_CASE("an order arriving before its login is refused, and the book is untouched") {
  auto host = fresh_session();
  client_stream stream;
  wire_log out;

  put_enter(stream, "EARLY0001", 100);
  std::size_t taken = 0;
  REQUIRE(host.offer(stream.view(), at_ms(0), out).get(taken) == dfr::error::ok);

  CHECK(host.phase() == venue::session_phase::ended);
  CHECK(host.ending() == venue::session_ending::protocol_error);
  CHECK(host.stats().out_of_phase == 1);
  // The point of refusing: a book mutated by an unidentified peer, with an acknowledgement that could
  // not say whose order it was.
  CHECK(host.orders().live_orders() == 0);
  CHECK(out.count() == 0);
}

TEST_CASE("an acknowledgement goes out as OUCH inside a numbered packet") {
  auto host = fresh_session();
  client_stream stream;
  wire_log out;
  log_in(host, stream, out, at_ms(0));
  out.clear();

  for (const auto* text : {"ORDER0001", "ORDER0002", "ORDER0003"}) {
    put_enter(stream, text, 100);
  }
  std::size_t taken = 0;
  REQUIRE(host.offer(stream.view(), at_ms(1), out).get(taken) == dfr::error::ok);
  CHECK(taken == stream.size());

  REQUIRE(out.count() == 3);
  CHECK(host.orders().live_orders() == 3);
  CHECK(host.stats().orders_in == 3);
  CHECK(host.stats().acknowledgements_out == 3);

  for (std::size_t i = 0; i < out.count(); ++i) {
    const auto frame = out.at(i);
    CHECK(frame.type == soup::packet_type::sequenced_data);
    ouch::accepted message;
    REQUIRE(ouch::decode_accepted(frame.payload).get(message) == dfr::error::ok);
  }
  CHECK(host.next_sequence() == 4);
}

TEST_CASE("a client counting the server's stream arrives at the numbers the server assigned") {
  auto host = fresh_session();
  client_stream stream;
  wire_log out;
  const auto accepted = log_in(host, stream, out, at_ms(0));
  out.clear();

  // The client starts counting from what Login Accepted told it, exactly as it would on a real
  // connection: the sequence is never on the wire, so agreement is the only evidence either side is
  // right. Neither component could assert this alone — one assigns, the other counts.
  soup::stream_cursor counting{accepted.next_sequence};

  for (const auto* text : {"COUNT0001", "COUNT0002"}) {
    put_enter(stream, text, 50);
  }
  std::size_t taken = 0;
  REQUIRE(host.offer(stream.view(), at_ms(1), out).get(taken) == dfr::error::ok);
  REQUIRE(out.count() == 2);

  const std::string replay = out.joined();
  std::size_t at = 0;
  std::vector<std::uint64_t> numbers;
  while (at < replay.size()) {
    dfr::packet_view rest;
    REQUIRE(dfr::packet_view{replay.data(), replay.size()}.subview(at, replay.size() - at).get(rest) ==
            dfr::error::ok);
    soup::sequenced_packet next;
    REQUIRE(counting.next(rest).get(next) == dfr::error::ok);
    numbers.push_back(next.sequence);
    at += next.frame.frame_size;
  }

  REQUIRE(numbers.size() == 2);
  CHECK(numbers[0] == 1);
  CHECK(numbers[1] == 2);
  CHECK(counting.next_sequence() == host.next_sequence());
}

TEST_CASE("a Sequenced Data Packet arriving from a client ends the session") {
  auto host = fresh_session();
  client_stream stream;
  wire_log out;
  log_in(host, stream, out, at_ms(0));
  out.clear();

  // Only a server may send this. It decodes perfectly and means nothing, which is exactly why a session
  // that shrugged at it would look healthy while being wired backwards.
  stream.put([](dfr::mutable_packet_view view) {
    static constexpr std::byte payload[1]{std::byte{0}};
    return soup::encode_packet(view, soup::packet_type::sequenced_data,
                               dfr::packet_view{payload, 1});
  });
  std::size_t taken = 0;
  REQUIRE(host.offer(stream.view(), at_ms(1), out).get(taken) == dfr::error::ok);

  CHECK(host.phase() == venue::session_phase::ended);
  CHECK(host.ending() == venue::session_ending::protocol_error);
  CHECK(host.stats().out_of_phase == 1);
}

TEST_CASE("a stream split at every byte boundary frames identically to one delivered whole") {
  auto whole = fresh_session();
  auto dribbled = fresh_session();
  client_stream setup;
  wire_log ignored;
  log_in(whole, setup, ignored, at_ms(0));
  setup.clear();
  ignored.clear();
  log_in(dribbled, setup, ignored, at_ms(0));

  client_stream orders;
  for (const auto* text : {"DRIP00001", "DRIP00002", "DRIP00003"}) {
    put_enter(orders, text, 10);
  }
  const std::string all = orders.bytes;

  wire_log at_once;
  std::size_t taken = 0;
  REQUIRE(whole.offer(dfr::packet_view{all.data(), all.size()}, at_ms(1), at_once).get(taken) ==
          dfr::error::ok);
  CHECK(taken == all.size());

  // A TCP stream splits wherever it likes, including in the middle of a length field. A session that
  // consumed a partial packet would lose the remainder of the connection, and nothing about that shows
  // up when the whole buffer arrives at once.
  wire_log byte_by_byte;
  std::string pending;
  for (const char byte : all) {
    pending.push_back(byte);
    std::size_t consumed = 0;
    REQUIRE(dribbled.offer(dfr::packet_view{pending.data(), pending.size()}, at_ms(1), byte_by_byte)
                .get(consumed) == dfr::error::ok);
    pending.erase(0, consumed);
  }
  CHECK(pending.empty());

  CHECK(dribbled.orders().live_orders() == whole.orders().live_orders());
  CHECK(byte_by_byte.count() == at_once.count());
  CHECK(dribbled.next_sequence() == whole.next_sequence());
  CHECK(byte_by_byte.joined() == at_once.joined());
}

TEST_CASE("an execution reaches the client on the same numbered stream as the acknowledgement") {
  auto host = fresh_session();
  client_stream stream;
  wire_log out;
  log_in(host, stream, out, at_ms(0));

  put_enter(stream, "FILL00001", 100);
  std::size_t taken = 0;
  REQUIRE(host.offer(stream.view(), at_ms(1), out).get(taken) == dfr::error::ok);
  out.clear();

  venue::order_outcome outcome{};
  REQUIRE(host.execute(token("FILL00001"), 40, at_dollars(150), at_ms(2), out).get(outcome) ==
          dfr::error::ok);
  CHECK(outcome == venue::order_outcome::executed);

  REQUIRE(out.count() == 1);
  CHECK(out.at(0).type == soup::packet_type::sequenced_data);
  ouch::executed message;
  REQUIRE(ouch::decode_executed(out.at(0).payload).get(message) == dfr::error::ok);
  CHECK(message.shares_this_fill == 40);
  // One stream and one counter, whoever the message originated with: the client's order produced
  // sequence 1 and the exchange's own execution produced sequence 2.
  CHECK(host.next_sequence() == 3);
}

TEST_CASE("executing on a session that never logged in is refused rather than quietly working") {
  auto host = fresh_session();
  wire_log out;

  venue::order_outcome outcome{};
  CHECK(host.execute(token("NOBODY001"), 10, at_dollars(1), at_ms(0), out).get(outcome) ==
        dfr::error::session_not_established);
  CHECK(out.count() == 0);
}

TEST_CASE("a logout ends the session, and nothing is sent back") {
  auto host = fresh_session();
  client_stream stream;
  wire_log out;
  log_in(host, stream, out, at_ms(0));
  out.clear();

  stream.put_bare(soup::packet_type::logout_request);
  std::size_t taken = 0;
  REQUIRE(host.offer(stream.view(), at_ms(1), out).get(taken) == dfr::error::ok);

  CHECK(host.phase() == venue::session_phase::ended);
  CHECK(host.ending() == venue::session_ending::logout_requested);
  // §3.6 gives no acknowledgement. Sending one would be a protocol extension nobody asked for.
  CHECK(out.count() == 0);
}

TEST_CASE("silence ends the session, and a heartbeat postpones it") {
  auto host = fresh_session();
  client_stream stream;
  wire_log out;
  log_in(host, stream, out, at_ms(0));

  host.poll(at_ms(10'000), out);
  CHECK(host.phase() == venue::session_phase::established);

  // A heartbeat is the whole of what holds a quiet connection open, so an order path that works and a
  // heartbeat path that does not still add up to a session that dies at lunchtime.
  stream.clear();
  stream.put_bare(soup::packet_type::client_heartbeat);
  std::size_t taken = 0;
  REQUIRE(host.offer(stream.view(), at_ms(10'000), out).get(taken) == dfr::error::ok);
  CHECK(host.stats().client_heartbeats == 1);

  host.poll(at_ms(20'000), out);
  CHECK(host.phase() == venue::session_phase::established);

  host.poll(at_ms(26'000), out);
  CHECK(host.phase() == venue::session_phase::ended);
  CHECK(host.ending() == venue::session_ending::client_silent);
}

TEST_CASE("a quiet server heartbeats on its own, on the interval and not on the poll") {
  auto host = fresh_session(/*heartbeats=*/true);
  client_stream stream;
  wire_log out;
  log_in(host, stream, out, at_ms(0));
  out.clear();

  host.poll(at_ms(1'200), out);
  CHECK(out.count_of(soup::packet_type::server_heartbeat) == 1);

  // Not again immediately: the interval runs from the last thing *sent*, not from the last poll. A
  // session polled in a tight loop would otherwise flood the connection it is trying to keep open.
  host.poll(at_ms(1'200), out);
  CHECK(out.count_of(soup::packet_type::server_heartbeat) == 1);

  host.poll(at_ms(2'400), out);
  CHECK(out.count_of(soup::packet_type::server_heartbeat) == 2);
}

TEST_CASE("a host closing the session says so once on the wire") {
  auto host = fresh_session();
  client_stream stream;
  wire_log out;
  log_in(host, stream, out, at_ms(0));
  out.clear();

  host.close(at_ms(1), out);
  REQUIRE(out.count() == 1);
  CHECK(out.at(0).type == soup::packet_type::end_of_session);
  CHECK(host.ending() == venue::session_ending::closed_by_host);

  // Twice sends one. A session that re-announced its own end would give a client two ends to reconcile.
  host.close(at_ms(2), out);
  CHECK(out.count() == 1);
}

TEST_CASE("a second login on an open session is refused rather than renumbering it") {
  auto host = fresh_session();
  client_stream stream;
  wire_log out;
  log_in(host, stream, out, at_ms(0));
  out.clear();

  stream.clear();
  stream.put_login();
  std::size_t taken = 0;
  REQUIRE(host.offer(stream.view(), at_ms(1), out).get(taken) == dfr::error::ok);

  // Answering it would leave two ideas of the sequence in play, and the client would believe the newer
  // one — which is how a session ends up numbering acknowledgements a client has already seen.
  CHECK(host.phase() == venue::session_phase::ended);
  CHECK(host.ending() == venue::session_ending::protocol_error);
  CHECK(out.count() == 0);
}

TEST_CASE("an unreadable order message ends the session rather than being skipped") {
  auto host = fresh_session();
  client_stream stream;
  wire_log out;
  log_in(host, stream, out, at_ms(0));
  out.clear();

  // A type byte that means nothing. OUCH carries no length inside the packet, so "ignore this one and
  // read the next" would be a guess about where the next one starts.
  static constexpr std::byte nonsense[4]{std::byte{'?'}, std::byte{0}, std::byte{0}, std::byte{0}};
  stream.put_order(dfr::packet_view{nonsense, 4});
  std::size_t taken = 0;
  REQUIRE(host.offer(stream.view(), at_ms(1), out).get(taken) == dfr::error::ok);

  CHECK(host.phase() == venue::session_phase::ended);
  CHECK(host.ending() == venue::session_ending::protocol_error);
}

TEST_CASE("nothing is emitted or accepted after the session has ended") {
  auto host = fresh_session();
  client_stream stream;
  wire_log out;
  log_in(host, stream, out, at_ms(0));

  stream.clear();
  stream.put_bare(soup::packet_type::logout_request);
  std::size_t taken = 0;
  REQUIRE(host.offer(stream.view(), at_ms(1), out).get(taken) == dfr::error::ok);
  stream.consume(taken);
  const auto after_logout = out.count();

  // Orders arriving after a logout are neither acknowledged nor consumed. A session that kept reading
  // would keep a book alive for a client that has gone.
  stream.clear();
  put_enter(stream, "GHOST0001", 100);
  std::size_t more = 0;
  REQUIRE(host.offer(stream.view(), at_ms(2), out).get(more) == dfr::error::ok);
  CHECK(more == 0);
  CHECK(out.count() == after_logout);
  CHECK(host.orders().live_orders() == 0);

  host.poll(at_ms(60'000), out);
  CHECK(out.count() == after_logout);
  CHECK(host.ending() == venue::session_ending::logout_requested);
}

TEST_CASE("the accounting balances across a whole session, and the sequence agrees with the count") {
  auto host = fresh_session();
  client_stream stream;
  wire_log out;
  log_in(host, stream, out, at_ms(0));

  for (const auto* text : {"BAL000001", "BAL000002", "BAL000003", "BAL000004"}) {
    put_enter(stream, text, 200);
  }
  std::size_t taken = 0;
  REQUIRE(host.offer(stream.view(), at_ms(1), out).get(taken) == dfr::error::ok);
  stream.clear();

  venue::order_outcome outcome{};
  REQUIRE(host.execute(token("BAL000001"), 200, at_dollars(150), at_ms(2), out).get(outcome) ==
          dfr::error::ok);
  REQUIRE(host.execute(token("BAL000002"), 75, at_dollars(150), at_ms(3), out).get(outcome) ==
          dfr::error::ok);

  const auto cancel = encoded([](dfr::mutable_packet_view view) {
    const ouch::cancel_order request{.token = token("BAL000003"), .intended_order_size = 0};
    return ouch::encode_cancel_order(view, request);
  });
  stream.put_order(dfr::packet_view{cancel.data(), cancel.size()});
  REQUIRE(host.offer(stream.view(), at_ms(4), out).get(taken) == dfr::error::ok);

  // Every share ever made liable is executed, canceled or still open — asserted once across a session
  // rather than after each step, which is what catches an accounting error that needs two paths to have
  // both run before it shows.
  CHECK(host.orders().accounts());
  // One acknowledgement per sequence number, no gaps and no reuse. If the session ever numbered
  // something it did not send, or sent something it did not number, these two disagree.
  CHECK(host.stats().acknowledgements_out == host.next_sequence() - 1);
}
