// The client target. The checks and the program live beside it, where they can be reviewed as a list.
#include "client_program.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  dfr_fuzz::run_client_program(dfr::packet_view{data, size});
  return 0;
}
