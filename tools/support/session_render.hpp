// Turning an OUCH acknowledgement into a line somebody can read.
//
// Split from tools/session.cpp at the seam it was over its length limit across, and a real one: a reader following
// the *script* a session runs never needs the field formatting, and the formatter has no opinion about the script.
//
// Everything here decodes before it prints. A tool that formatted bytes at offsets it knew would agree with itself
// about a layout that was wrong: the same rule the venue emitters follow, applied to output.

#ifndef DFR_TOOLS_SUPPORT_SESSION_RENDER_HPP
#define DFR_TOOLS_SUPPORT_SESSION_RENDER_HPP

#include <dfr/core/packet_view.hpp>
#include <dfr/wire/ouch.hpp>

#include <cstdio>
#include <string>

namespace dfr_tools {

namespace ouch = dfr::wire::ouch;

// What an outbound OUCH message says, decoded rather than read at offsets.
struct described {
  std::string name;
  std::string detail;
};

inline described describe_ouch(dfr::packet_view message) {
  if (message.empty()) {
    return {"(empty)", ""};
  }
  char detail[160];
  switch (static_cast<ouch::outbound_type>(message.u8_at(0))) {
    case ouch::outbound_type::accepted: {
      ouch::accepted decoded;
      if (ouch::decode_accepted(message).get(decoded) != dfr::error::ok) {
        break;
      }
      const auto text = decoded.token.text();
      std::snprintf(detail, sizeof detail, "token=%.*s shares=%u state=%c",
                    static_cast<int>(text.size()), text.data(), decoded.shares_accepted,
                    static_cast<char>(decoded.order.state));
      return {"Accepted", detail};
    }
    case ouch::outbound_type::executed: {
      ouch::executed decoded;
      if (ouch::decode_executed(message).get(decoded) != dfr::error::ok) {
        break;
      }
      const auto text = decoded.token.text();
      std::snprintf(detail, sizeof detail, "token=%.*s shares=%u match=%llu",
                    static_cast<int>(text.size()), text.data(), decoded.shares_this_fill,
                    static_cast<unsigned long long>(decoded.match_number));
      return {"Executed", detail};
    }
    case ouch::outbound_type::canceled: {
      ouch::canceled decoded;
      if (ouch::decode_canceled(message).get(decoded) != dfr::error::ok) {
        break;
      }
      const auto text = decoded.token.text();
      const auto why = ouch::name_of_cancel_reason(decoded.reason);
      std::snprintf(detail, sizeof detail, "token=%.*s removed=%u reason=%.*s",
                    static_cast<int>(text.size()), text.data(), decoded.shares_decremented,
                    static_cast<int>(why.size()), why.data());
      return {"Canceled", detail};
    }
    default:
      break;
  }
  std::snprintf(detail, sizeof detail, "%zu bytes", message.size());
  return {std::string{"type '"} + static_cast<char>(message.u8_at(0)) + "'", detail};
}

}  // namespace dfr_tools

#endif  // DFR_TOOLS_SUPPORT_SESSION_RENDER_HPP
