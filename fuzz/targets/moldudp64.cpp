// The moldudp64 target. Ten lines, because the checks live in fuzz/checks.hpp where they can be reviewed together.
#include "checks.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  dfr_fuzz::fuzz_moldudp64(dfr::packet_view{data, size});
  return 0;
}
