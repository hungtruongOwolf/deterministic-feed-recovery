#include "checks.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  dfr_fuzz::fuzz_itch(dfr::packet_view{data, size});
  return 0;
}
