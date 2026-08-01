// The session's phases: who may say what, and when.
//
// Login, refusal, logout, silence, and everything that is illegal for the phase it arrives in. Split from
// order_session_flow_test.cpp at a real seam rather than a line count: a reader checking who may speak when never
// needs the sequence arithmetic, and a reader checking the sequence arithmetic never needs the phase table.
//
// The joins are what both files are about: a join is where a defect lives without either side being wrong. These
// are the joins about *authority*: what a server does with a packet only a server may send, and what becomes of an
// order that arrives before the login it needs.

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
  // connection: with both halves still passing their own tests.
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

TEST_CASE("a Sequenced Data Packet arriving from a client ends the session") {
  auto host = fresh_session();
  client_stream stream;
  wire_log out;
  log_in(host, stream, out, at_ms(0));
  out.clear();

  // Only a server may send this. It decodes perfectly and means nothing, which is exactly why a session
  // that shrugged at it would look healthy while being wired backwards.
  stream.put([](dfr::mutable_packet_view view) {
    static constexpr std::byte kPayload[1]{std::byte{0}};
    return soup::encode_packet(view, soup::packet_type::sequenced_data,
                               dfr::packet_view{kPayload, 1});
  });
  std::size_t taken = 0;
  REQUIRE(host.offer(stream.view(), at_ms(1), out).get(taken) == dfr::error::ok);

  CHECK(host.phase() == venue::session_phase::ended);
  CHECK(host.ending() == venue::session_ending::protocol_error);
  CHECK(host.stats().out_of_phase == 1);
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
  // one, which is how a session ends up numbering acknowledgements a client has already seen.
  CHECK(host.phase() == venue::session_phase::ended);
  CHECK(host.ending() == venue::session_ending::protocol_error);
  CHECK(out.count() == 0);
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
