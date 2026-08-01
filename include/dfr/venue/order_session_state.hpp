// What phase an order-entry session is in, what it was configured with, and what it has carried.
//
// Split from order_session.hpp on the seam docs/STYLE.md §1.10 asks for, the same one order_outcome.hpp
// and recovery::client_state use: a test asserting on a phase, or a caller logging session counts, needs
// this vocabulary and never needs the state machine.

#ifndef DFR_VENUE_ORDER_SESSION_STATE_HPP
#define DFR_VENUE_ORDER_SESSION_STATE_HPP

#include <dfr/core/assert.hpp>
#include <dfr/wire/soupbintcp.hpp>

#include <cstdint>
#include <string_view>

namespace dfr::inline v1::venue {

// The three phases a SoupBinTCP session on the server side can be in.
//
// Three, not four: there is no "logging in". A Login Request is answered in the same call that reads
// it, so the intermediate state would exist for no instructions and be a place for a bug to live.
enum class session_phase : std::uint8_t {
  // Nothing but a Login Request is legal. Anything else is a client wired wrong, and the session says
  // so rather than guessing what was meant.
  awaiting_login,

  // Logged in. Order entry flows, heartbeats flow both ways.
  established,

  // Over, by logout, by timeout, or by a protocol error. A session never reopens: the counts survive
  // so that a caller can report what the connection carried, but nothing else is accepted.
  ended,

  count_
};

[[nodiscard]] constexpr std::string_view name_of(session_phase value) noexcept {
  switch (value) {
    case session_phase::awaiting_login: return "awaiting_login";
    case session_phase::established:    return "established";
    case session_phase::ended:          return "ended";
    case session_phase::count_:         break;
  }
  DFR_UNREACHABLE("unnamed session phase");
}

// Why a session ended, which a caller almost always wants and can never reconstruct.
//
// `not_ended` is a value rather than an absence because a session that has not ended still has to
// answer the question, and returning "none" as an error would make the ordinary case exceptional.
enum class session_ending : std::uint8_t {
  not_ended,
  // The client asked, per §3.6. No reply is sent: the specification's Logout Request is not
  // acknowledged, and inventing an acknowledgement would be a protocol extension.
  logout_requested,
  // Nothing arrived for kSilenceTimeoutMillis. §4: the connection is presumed dead.
  client_silent,
  // The client sent something illegal for its phase: a Sequenced Data Packet, say, which only a
  // server may send. Ending is the honest answer; carrying on would be guessing.
  protocol_error,
  // The host asked to close the session, and an End Of Session packet went out.
  closed_by_host,

  count_
};

[[nodiscard]] constexpr std::string_view name_of(session_ending value) noexcept {
  switch (value) {
    case session_ending::not_ended:        return "not_ended";
    case session_ending::logout_requested: return "logout_requested";
    case session_ending::client_silent:    return "client_silent";
    case session_ending::protocol_error:   return "protocol_error";
    case session_ending::closed_by_host:   return "closed_by_host";
    case session_ending::count_:           break;
  }
  DFR_UNREACHABLE("unnamed session ending");
}

struct order_session_options {
  // What this server calls its session. Ten characters on the wire, and echoed in Login Accepted so a
  // reconnecting client can tell whether it is rejoining the same day.
  std::string_view session_id{"DFRSESSION"};

  // Six and ten characters on the wire. The defaults fit, and the constructor asserts that whatever a
  // caller supplies fits too: a username longer than its field can never appear in a Login Request, so a
  // session configured with one would refuse every login forever and never say why.
  std::string_view username{"DFRUSR"};
  std::string_view password{"DFRPASS"};

  // The sequence the server's own outbound stream starts at. One, per the specification's numbering,
  // and configurable because a session resumed after a reconnect does not start at one.
  std::uint64_t first_sequence{1};

  // Whether to send server heartbeats when the outbound side is idle. On by default because §4 requires
  // it, and switchable because a test that asserts on exact bytes does not want them appearing.
  bool send_heartbeats{true};
};

struct order_session_stats {
  std::uint64_t packets_in{0};
  std::uint64_t packets_out{0};
  // Unsequenced Data Packets carrying an order message. The number that matters, as distinct from the
  // heartbeats that make up most of the traffic.
  std::uint64_t orders_in{0};
  // Sequenced Data Packets the server sent, each carrying one OUCH acknowledgement.
  std::uint64_t acknowledgements_out{0};
  std::uint64_t client_heartbeats{0};
  std::uint64_t server_heartbeats{0};
  // Login Requests refused, which is not the same as connections refused: a client may retry.
  std::uint64_t logins_rejected{0};
  // Packets that decoded but were illegal for the phase they arrived in.
  std::uint64_t out_of_phase{0};

  [[nodiscard]] friend constexpr bool operator==(const order_session_stats&,
                                                 const order_session_stats&) = default;
};

}  // namespace dfr::inline v1::venue
#endif  // DFR_VENUE_ORDER_SESSION_STATE_HPP
