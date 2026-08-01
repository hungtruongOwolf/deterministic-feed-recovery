// An OUCH order-entry session: the seam between a SoupBinTCP stream and an order-entry host.
//
// Both halves of this existed and nothing joined them, which is the shape of defect this project exists
// to argue about. wire::soupbintcp framed a stream and counted the sequence the protocol never puts on
// the wire; wire::ouch decoded and encoded order messages; venue::order_entry decided what happens to an
// order. Each was tested and each was correct. The questions none of them could answer:
//
//   * who assigns the sequence number to an acknowledgement, and when;
//   * what a server does with a Sequenced Data Packet arriving *from* a client;
//   * whether a login is answered before or after the session starts counting;
//   * what happens to an order message that arrives before the login it needs.
//
// Those are session questions, and a component that does not exist cannot get them wrong in a way anyone
// can see. So this is the component, and the tests are about the joins rather than about either half.
//
// Poll-driven, like recovery::client
// ----------------------------------
// Nothing here calls back into the caller except to emit bytes, and nothing schedules. `offer` consumes
// what it can and says how much; `poll` is how time passes. A session that reacted to a callback would
// need the caller to be reentrant at exactly the moment it is mutating a book.

#ifndef DFR_VENUE_ORDER_SESSION_HPP
#define DFR_VENUE_ORDER_SESSION_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>
#include <dfr/core/clock.hpp>
#include <dfr/core/error.hpp>
#include <dfr/core/mutable_packet_view.hpp>
#include <dfr/core/packet_view.hpp>
#include <dfr/core/result.hpp>
#include <dfr/venue/order_entry.hpp>
#include <dfr/venue/order_session_state.hpp>
#include <dfr/venue/order_session_writer.hpp>
#include <dfr/wire/ouch.hpp>
#include <dfr/wire/soupbintcp.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace dfr::inline v1 {
namespace venue {

template <clock_source Clock>
class order_session {
 public:
  using time_point = typename Clock::time_point;

  constexpr order_session(order_session_options options,
                          order_entry_options entry_options) noexcept
      : options_(options), entry_(entry_options), writer_(options.first_sequence) {
    // A credential that cannot be written into its field can never be read out of one, so a session
    // configured with it would reject every login and look like an authentication problem.
    DFR_ASSERT(options.username.size() <= wire::soupbintcp::kUsernameSize,
               "the configured username does not fit the Login Request field");
    DFR_ASSERT(options.password.size() <= wire::soupbintcp::kPasswordSize,
               "the configured password does not fit the Login Request field");
    DFR_ASSERT(options.session_id.size() <= wire::soupbintcp::kSessionSize,
               "the configured session identifier does not fit the Login Accepted field");
  }

  [[nodiscard]] constexpr session_phase phase() const noexcept { return phase_; }
  [[nodiscard]] constexpr session_ending ending() const noexcept { return ending_; }
  [[nodiscard]] constexpr bool established() const noexcept {
    return phase_ == session_phase::established;
  }

  // Fills in the two counters the writer owns before handing the struct over, so a caller sees one set of
  // numbers rather than having to add two objects together.
  [[nodiscard]] constexpr const order_session_stats& stats() const noexcept DFR_LIFETIME_BOUND {
    stats_.packets_out = writer_.packets_out();
    stats_.acknowledgements_out = writer_.acknowledgements_out();
    return stats_;
  }
  [[nodiscard]] constexpr const order_entry<Clock>& orders() const noexcept
      DFR_LIFETIME_BOUND {
    return entry_;
  }

  // The sequence the *next* outbound Sequenced Data Packet will carry: the same convention Login
  // Accepted uses, so the number a client is told and the number this holds are the same number.
  [[nodiscard]] constexpr std::uint64_t next_sequence() const noexcept {
    return writer_.next_sequence();
  }

  // ---- input -------------------------------------------------------------

  // Consumes whole packets from the front of `stream` and returns how many bytes were taken.
  //
  // A partial packet at the end is left alone and reported as not consumed, so the caller appends more
  // bytes and calls again with the same buffer. That is the same discipline soupbintcp::stream_cursor
  // and capture::pcap::reader follow, and for the same reason: a read that consumed what it could not
  // parse would leave nothing behind to retry with.
  //
  // Returns an error only for a stream this session cannot go on reading. A packet that is merely
  // *illegal* ends the session and returns ok with the bytes consumed, because the caller's next job is
  // to close a connection, not to handle an exception.
  template <typename Emit>
  [[nodiscard]] constexpr result<std::size_t> offer(packet_view stream, time_point now,
                                                    Emit&& emit) noexcept {
    std::size_t taken = 0;
    while (taken < stream.size() && phase_ != session_phase::ended) {
      packet_view rest;
      if (const auto err = stream.subview(taken, stream.size() - taken).get(rest);
          err != error::ok) DFR_UNLIKELY {
        return err;
      }

      wire::soupbintcp::packet frame;
      if (const auto err = wire::soupbintcp::decode(rest).get(frame); err != error::ok) {
        // Not enough bytes is the ordinary case on a stream, not a failure: stop here and keep what
        // is left for the next call.
        if (err == error::need_more_bytes) {
          break;
        }
        // Anything else is a stream this session cannot resynchronise on. SoupBinTCP has no framing
        // marker to hunt for, so "skip forward until it parses" would be inventing one.
        end(session_ending::protocol_error, now, emit);
        return err;
      }

      taken += frame.frame_size;
      ++stats_.packets_in;
      last_in_ = now;
      received_ = true;

      if (const auto err = dispatch(frame, now, emit); err != error::ok) DFR_UNLIKELY {
        return err;
      }
    }
    return taken;
  }

  // ---- time --------------------------------------------------------------

  // Sends a heartbeat if the outbound side has been quiet, and ends the session if the inbound side has
  // been quiet for too long.
  //
  // Both are §4's numbers rather than a tuning choice, which is why they are constants in the wire layer
  // and not options here.
  template <typename Emit>
  constexpr void poll(time_point now, Emit&& emit) noexcept {
    if (phase_ == session_phase::ended) {
      return;
    }

    if (received_ && millis_between(last_in_, now) >= wire::soupbintcp::kSilenceTimeoutMillis) {
      end(session_ending::client_silent, now, emit);
      return;
    }

    // Only an established session heartbeats. Before login there is nothing to keep alive, and a server
    // beating at a client that has not identified itself is talking to a port scanner.
    if (options_.send_heartbeats && phase_ == session_phase::established &&
        millis_between(last_out_, now) >= wire::soupbintcp::kHeartbeatIntervalMillis) {
      writer_.bare(wire::soupbintcp::packet_type::server_heartbeat, emit);
      last_out_ = now;
      ++stats_.server_heartbeats;
    }
  }

  // The host ends the session, with the End Of Session packet §3.5 requires.
  template <typename Emit>
  constexpr void close(time_point now, Emit&& emit) noexcept {
    if (phase_ == session_phase::ended) {
      return;
    }
    writer_.bare(wire::soupbintcp::packet_type::end_of_session, emit);
    last_out_ = now;
    end(session_ending::closed_by_host, now, emit);
  }

  // ---- the matching engine's side ---------------------------------------

  // Fills part or all of a live order. Driven by the caller because there is no matching engine here:
  // see order_entry.hpp for why that is a scope decision and not an omission.
  template <typename Emit>
  [[nodiscard]] constexpr result<order_outcome> execute(const wire::ouch::order_token& token,
                                                        std::uint32_t shares,
                                                        wire::ouch::price at, time_point now,
                                                        Emit&& emit) noexcept {
    if (phase_ != session_phase::established) DFR_UNLIKELY {
      return error::session_not_established;
    }
    return entry_.execute(token, shares, at, now, sink(emit));
  }

 private:
  template <typename Emit>
  [[nodiscard]] constexpr error dispatch(const wire::soupbintcp::packet& frame, time_point now,
                                         Emit&& emit) noexcept {
    using wire::soupbintcp::packet_type;

    // A packet only a server may send, arriving from a client, is a session wired backwards. It decodes
    // perfectly and means nothing, which is exactly the failure worth naming.
    if (wire::soupbintcp::from_server(frame.type)) DFR_UNLIKELY {
      ++stats_.out_of_phase;
      end(session_ending::protocol_error, now, emit);
      return error::ok;
    }

    switch (frame.type) {
      case packet_type::login_request:
        return on_login(frame, now, emit);

      case packet_type::client_heartbeat:
        // Nothing to do but be alive, which `last_in_` already recorded. §4 asks for no reply.
        if (phase_ != session_phase::established) DFR_UNLIKELY {
          ++stats_.out_of_phase;
          end(session_ending::protocol_error, now, emit);
          return error::ok;
        }
        ++stats_.client_heartbeats;
        return error::ok;

      case packet_type::logout_request:
        // §3.6 gives no acknowledgement, and inventing one would be a protocol extension.
        end(session_ending::logout_requested, now, emit);
        return error::ok;

      case packet_type::unsequenced_data:
        return on_order(frame, now, emit);

      default:
        ++stats_.out_of_phase;
        end(session_ending::protocol_error, now, emit);
        return error::ok;
    }
  }

  template <typename Emit>
  [[nodiscard]] constexpr error on_login(const wire::soupbintcp::packet& frame, time_point now,
                                         Emit&& emit) noexcept {
    // A second login on an established session is not a re-login; it is a client that lost track of
    // where it was. Answering it would leave two ideas of the sequence in play.
    if (phase_ != session_phase::awaiting_login) DFR_UNLIKELY {
      ++stats_.out_of_phase;
      end(session_ending::protocol_error, now, emit);
      return error::ok;
    }

    wire::soupbintcp::login_request request;
    if (const auto err = wire::soupbintcp::decode_login_request(frame.payload).get(request);
        err != error::ok) DFR_UNLIKELY {
      end(session_ending::protocol_error, now, emit);
      return error::ok;
    }

    if (request.username != options_.username || request.password != options_.password) {
      return reject(wire::soupbintcp::reject_reason::not_authorized, now, emit);
    }

    // An empty requested session means "whatever is current", per §3.2: the absence is the meaning, so
    // it matches rather than failing to.
    if (!request.requested_session.empty() && request.requested_session != options_.session_id) {
      return reject(wire::soupbintcp::reject_reason::session_not_available, now, emit);
    }

    // §3.2: requesting 1 means "from the beginning of the session", and anything else, including 0,
    // which means "from the next message you send": starts where the server already is. This server
    // retains nothing, so a replay request is answered with the truth about where it is rather than with
    // a rejection: the client learns the position from the Login Accepted it is about to read.
    if (request.requested_sequence == 1 && options_.first_sequence == 1) {
      writer_.restart_at(options_.first_sequence);
    }

    phase_ = session_phase::established;
    if (const auto err = writer_.login_accepted(options_.session_id, emit); !err) DFR_UNLIKELY {
      return err.error_code();
    }
    last_out_ = now;
    return error::ok;
  }

  template <typename Emit>
  [[nodiscard]] constexpr error on_order(const wire::soupbintcp::packet& frame, time_point now,
                                         Emit&& emit) noexcept {
    // An order before its login. Refusing is the point: the alternative is a book mutated by an
    // unidentified peer, and no acknowledgement could name who it belonged to.
    if (phase_ != session_phase::established) DFR_UNLIKELY {
      ++stats_.out_of_phase;
      end(session_ending::protocol_error, now, emit);
      return error::ok;
    }
    if (frame.payload.empty()) DFR_UNLIKELY {
      ++stats_.out_of_phase;
      end(session_ending::protocol_error, now, emit);
      return error::ok;
    }

    ++stats_.orders_in;
    auto numbered = sink(emit);

    // One routing table rather than four copies of the same five lines. Each branch decodes into its own type and
    // hands it to its own method, so the shape is identical every time and the only thing that varies is a pair of
    // names, which is exactly what a template parameter is for.
    switch (static_cast<wire::ouch::inbound_type>(frame.payload.u8_at(0))) {
      case wire::ouch::inbound_type::enter_order:
        return route<wire::ouch::enter_order>(frame.payload, now, emit, numbered,
                                              wire::ouch::decode_enter_order,
                                              [&](const auto& r, auto&& s) { return entry_.enter(r, now, s); });
      case wire::ouch::inbound_type::replace_order:
        return route<wire::ouch::replace_order>(frame.payload, now, emit, numbered,
                                                wire::ouch::decode_replace_order,
                                                [&](const auto& r, auto&& s) { return entry_.replace(r, now, s); });
      case wire::ouch::inbound_type::cancel_order:
        return route<wire::ouch::cancel_order>(frame.payload, now, emit, numbered,
                                               wire::ouch::decode_cancel_order,
                                               [&](const auto& r, auto&& s) { return entry_.cancel(r, now, s); });
      case wire::ouch::inbound_type::modify_order:
        return route<wire::ouch::modify_order>(frame.payload, now, emit, numbered,
                                               wire::ouch::decode_modify_order,
                                               [&](const auto& r, auto&& s) { return entry_.modify(r, now, s); });
    }
    return malformed(now, emit);
  }

  // Decode, then act, then report: the shape every inbound message shares.
  template <typename Request, typename Emit, typename Sink, typename Decode, typename Act>
  [[nodiscard]] constexpr error route(packet_view body, time_point now, Emit&& emit, Sink&& sink,
                                      Decode&& decode, Act&& act) noexcept {
    Request request;
    if (const auto err = decode(body).get(request); err != error::ok) {
      return malformed(now, emit);
    }
    // An outcome is not an error: `ignored` is a legitimate answer the specification requires, so only a failure
    // to *produce* an answer propagates.
    return act(request, sink).error_code();
  }

  // An order message this host cannot read. The session ends rather than skipping it: OUCH carries no
  // length of its own inside the packet, so "ignore this one" is a guess about what the next one is.
  template <typename Emit>
  [[nodiscard]] constexpr error malformed(time_point now, Emit&& emit) noexcept {
    ++stats_.out_of_phase;
    end(session_ending::protocol_error, now, emit);
    return error::ok;
  }


  // Everything order_entry emits, numbered and framed. One lambda rather than a member, because the writer needs
  // no notion of "now" and the session's clock has no business in a frame builder.
  template <typename Emit>
  [[nodiscard]] constexpr auto sink(Emit&& emit) noexcept {
    return [this, &emit](packet_view message) noexcept { writer_.sequenced(message, emit); };
  }

  // A rejection is a reply and an ending, and §3.3 says the server closes afterwards, so a client retries by
  // reconnecting rather than by waiting.
  template <typename Emit>
  [[nodiscard]] constexpr error reject(wire::soupbintcp::reject_reason reason, time_point now,
                                       Emit&& emit) noexcept {
    ++stats_.logins_rejected;
    writer_.login_rejected(reason, emit);
    last_out_ = now;
    ending_ = session_ending::protocol_error;
    phase_ = session_phase::ended;
    return error::ok;
  }

  template <typename Emit>
  constexpr void end(session_ending why, time_point now, Emit&& emit) noexcept {
    if (phase_ == session_phase::ended) {
      return;
    }
    (void)now;
    (void)emit;
    phase_ = session_phase::ended;
    ending_ = why;
  }

  [[nodiscard]] static constexpr std::int64_t millis_between(time_point from,
                                                             time_point to) noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
  }

  order_session_options options_{};
  order_entry<Clock> entry_;
  order_session_writer writer_;
  session_phase phase_{session_phase::awaiting_login};
  session_ending ending_{session_ending::not_ended};
  mutable order_session_stats stats_{};

  time_point last_in_{};
  time_point last_out_{};
  // Whether anything has arrived at all. Without it, a session created at time zero and polled at
  // sixteen seconds would time out having never had a chance to be spoken to.
  bool received_{false};
};

}  // namespace venue
}  // namespace dfr::inline v1

#endif  // DFR_VENUE_ORDER_SESSION_HPP
