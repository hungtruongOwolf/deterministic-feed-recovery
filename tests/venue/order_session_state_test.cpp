// The vocabulary itself: every phase and every ending, named.
//
// Found by measuring coverage rather than by reading: name_of(session_phase) and name_of(session_ending) are
// called only from tools/session.cpp, never from a test, which makes them the one pair of enum-to-string
// functions in this codebase without the direct unit test every sibling has (arbiter's, the client's,
// requester's, snapshot's). That asymmetry is a real gap rather than a stylistic one: these two strings are
// also what tools/support/trace_writer.hpp puts into the JSON the viewer reads, so a swapped case in the
// switch would show up as a wrong word on the live page with nothing here to catch it first.

#include <dfr/venue/order_session_state.hpp>

#include <catch2/catch_test_macros.hpp>

namespace venue = dfr::venue;

TEST_CASE("every session phase has its own name", "[venue][order_session]") {
  CHECK(venue::name_of(venue::session_phase::awaiting_login) == "awaiting_login");
  CHECK(venue::name_of(venue::session_phase::established) == "established");
  CHECK(venue::name_of(venue::session_phase::ended) == "ended");
}

TEST_CASE("every session ending has its own name", "[venue][order_session]") {
  CHECK(venue::name_of(venue::session_ending::not_ended) == "not_ended");
  CHECK(venue::name_of(venue::session_ending::logout_requested) == "logout_requested");
  CHECK(venue::name_of(venue::session_ending::client_silent) == "client_silent");
  CHECK(venue::name_of(venue::session_ending::protocol_error) == "protocol_error");
  CHECK(venue::name_of(venue::session_ending::closed_by_host) == "closed_by_host");
}
