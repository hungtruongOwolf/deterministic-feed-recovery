// OUCH 4.2: NASDAQ order entry, carried over SoupBinTCP.
//
// Umbrella header. Inbound messages travel as SoupBinTCP Unsequenced Data and outbound as Sequenced
// Data, so `dfr/wire/soupbintcp.hpp` frames what this decodes.
//
// The layout here is transcribed from the specification and, unlike the IEX-TP layout in this
// repository, **has not been verified against real traffic**: NASDAQ publishes no OUCH captures and a
// live session needs an exchange relationship. Each message's total length is pinned by a
// static_assert so a mistranscribed offset is an arithmetic disagreement rather than a silent misread.

#ifndef DFR_WIRE_OUCH_HPP
#define DFR_WIRE_OUCH_HPP

#include <dfr/wire/ouch/constants.hpp>
#include <dfr/wire/ouch/enums.hpp>
#include <dfr/wire/ouch/inbound.hpp>
#include <dfr/wire/ouch/inbound_encode.hpp>
#include <dfr/wire/ouch/outbound_ack.hpp>
#include <dfr/wire/ouch/outbound_encode.hpp>
#include <dfr/wire/ouch/outbound_events.hpp>
#include <dfr/wire/ouch/price.hpp>
#include <dfr/wire/ouch/token.hpp>

#endif  // DFR_WIRE_OUCH_HPP
