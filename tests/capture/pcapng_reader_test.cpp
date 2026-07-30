#include <dfr/capture/pcapng.hpp>

#include "capture/support/pcapng_file.hpp"
#include "support/death_test.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace ng = dfr::capture::pcapng;
using dfr_test::pcapng::as_text;
using dfr_test::pcapng::file_builder;
using dfr_test::pcapng::minimal;

namespace {

// Reads every frame, returning what arrived plus the outcome.
struct drained {
  std::vector<std::string_view> payloads;
  std::vector<std::uint64_t> timestamps;
  dfr::error outcome{dfr::error::ok};
};

drained drain_all(ng::reader& r) {
  drained out;
  const auto result = r.drain([&](const dfr::capture::frame& f) {
    out.payloads.push_back(as_text(f.data));
    out.timestamps.push_back(f.timestamp_ns);
  });
  out.outcome = result.error_code();
  return out;
}

}  // namespace

TEST_CASE("a minimal file yields its packet", "[capture][pcapng]") {
  const auto f = minimal("hello");

  ng::reader r;
  REQUIRE(ng::reader::over(f.view()).get(r) == dfr::error::ok);
  CHECK(r.sections() == 1);
  CHECK(r.interfaces() == 0);  // the IDB has not been reached yet

  const auto got = drain_all(r);
  CHECK(got.outcome == dfr::error::ok);
  CHECK(got.payloads == std::vector<std::string_view>{"hello"});
  CHECK(r.frames_read() == 1);
  CHECK(r.interfaces() == 1);
  CHECK(r.done());
}

TEST_CASE("both byte orders read identically", "[capture][pcapng]") {
  // The SHB's byte-order magic settles the order for the whole section, and its
  // own Block Total Length is written in that order — which is why the format
  // gives the SHB a palindromic type.
  const auto little = minimal("payload", /*little=*/true);
  const auto big = minimal("payload", /*little=*/false);

  ng::reader rl;
  ng::reader rb;
  REQUIRE(ng::reader::over(little.view()).get(rl) == dfr::error::ok);
  REQUIRE(ng::reader::over(big.view()).get(rb) == dfr::error::ok);

  const auto gl = drain_all(rl);
  const auto gb = drain_all(rb);
  CHECK(gl.payloads == gb.payloads);
  CHECK(gl.timestamps == gb.timestamps);
  CHECK(gl.outcome == dfr::error::ok);
  CHECK(gb.outcome == dfr::error::ok);
}

TEST_CASE("a file not beginning with a section header is refused",
          "[capture][pcapng]") {
  file_builder f;
  f.interface_description();  // no SHB first
  const auto opened = ng::reader::over(f.view());

  CHECK_FALSE(opened.has_value());
  CHECK(opened.error_code() == dfr::error::not_supported);
}

TEST_CASE("an unsupported major version is refused", "[capture][pcapng]") {
  file_builder f;
  f.section(/*major=*/2).interface_description().packet(0, "x");

  const auto opened = ng::reader::over(f.view());
  CHECK_FALSE(opened.has_value());
  CHECK(opened.error_code() == dfr::error::not_supported);
}

TEST_CASE("the timestamp resolution comes from the interface",
          "[capture][pcapng][regression]") {
  // The same tick count means different times depending on the interface's
  // if_tsresol. A reader that assumes microseconds is wrong by a thousand on the
  // nanosecond files, and the answer is per interface rather than per file — one
  // layer further from the data than classic pcap's magic.
  const auto micros = minimal("x", true, /*tsresol=*/6);
  const auto nanos = minimal("x", true, /*tsresol=*/9);
  const auto absent = minimal("x", true, /*tsresol=*/-1);

  // All three open and drain cleanly; the point is what one tick resolves to.
  for (const auto* variant : {&micros, &nanos, &absent}) {
    ng::reader r;
    REQUIRE(ng::reader::over(variant->view()).get(r) == dfr::error::ok);
    CHECK(drain_all(r).outcome == dfr::error::ok);
  }

  file_builder one_micro{true};
  one_micro.section().interface_description(1, 65535, 6).packet(1, "x");
  file_builder one_nano{true};
  one_nano.section().interface_description(1, 65535, 9).packet(1, "x");
  file_builder one_default{true};
  one_default.section().interface_description(1, 65535, -1).packet(1, "x");

  ng::reader rm;
  ng::reader rn;
  ng::reader rd;
  REQUIRE(ng::reader::over(one_micro.view()).get(rm) == dfr::error::ok);
  REQUIRE(ng::reader::over(one_nano.view()).get(rn) == dfr::error::ok);
  REQUIRE(ng::reader::over(one_default.view()).get(rd) == dfr::error::ok);

  CHECK(drain_all(rm).timestamps == std::vector<std::uint64_t>{1'000});
  CHECK(drain_all(rn).timestamps == std::vector<std::uint64_t>{1});
  CHECK(drain_all(rd).timestamps == std::vector<std::uint64_t>{1'000});
}

TEST_CASE("a 64-bit timestamp is reassembled from its two halves",
          "[capture][pcapng]") {
  const std::uint64_t ticks = 0x0000'0001'2345'6789ULL;
  file_builder f{true};
  f.section().interface_description(1, 65535, 9).packet(ticks, "x");

  ng::reader r;
  REQUIRE(ng::reader::over(f.view()).get(r) == dfr::error::ok);
  CHECK(drain_all(r).timestamps == std::vector<std::uint64_t>{ticks});
}

TEST_CASE("unknown blocks are skipped, not refused", "[capture][pcapng]") {
  // Tolerating unrecognised blocks is the whole reason the format carries a
  // length. A reader that refuses them cannot read a file written by a newer
  // tool, which is most files eventually.
  file_builder f{true};
  f.section()
      .interface_description()
      .unknown_block()
      .packet(0, "first")
      .unknown_block(0x0000'0BAE)
      .packet(0, "second")
      .unknown_block();

  ng::reader r;
  REQUIRE(ng::reader::over(f.view()).get(r) == dfr::error::ok);

  const auto got = drain_all(r);
  CHECK(got.outcome == dfr::error::ok);
  CHECK(got.payloads == std::vector<std::string_view>{"first", "second"});
  CHECK(r.blocks_skipped() == 3);
  CHECK(r.done());
}

TEST_CASE("a new section resets the interface table",
          "[capture][pcapng][regression]") {
  // IEX HIST files are merged and carry several sections. An interface id is only
  // meaningful within its own section, so carrying the table across would attach
  // the wrong timestamp resolution and link type to real packets.
  file_builder f{true};
  f.section()
      .interface_description(1, 65535, /*tsresol=*/6)
      .packet(1, "micros")
      .section()
      .interface_description(1, 65535, /*tsresol=*/9)
      .packet(1, "nanos");

  ng::reader r;
  REQUIRE(ng::reader::over(f.view()).get(r) == dfr::error::ok);

  const auto got = drain_all(r);
  CHECK(got.outcome == dfr::error::ok);
  CHECK(got.payloads == std::vector<std::string_view>{"micros", "nanos"});
  // The proof the table was reset: the same tick count resolves differently.
  CHECK(got.timestamps == std::vector<std::uint64_t>{1'000, 1});
  CHECK(r.sections() == 2);
  CHECK(r.interfaces() == 1);  // one per section, not accumulated
}

TEST_CASE("a packet naming an undescribed interface is refused",
          "[capture][pcapng]") {
  // Defaulting to interface 0 would attach a guessed timestamp resolution to real
  // data, which is worse than refusing.
  file_builder f{true};
  f.section().interface_description().packet(0, "x", /*interface_id=*/3);

  ng::reader r;
  REQUIRE(ng::reader::over(f.view()).get(r) == dfr::error::ok);

  const auto got = drain_all(r);
  CHECK(got.outcome == dfr::error::invalid_argument);
  CHECK(got.payloads.empty());
  CHECK_FALSE(r.done());  // the failed block was not consumed
}

TEST_CASE("a simple packet block is refused rather than timestamped as zero",
          "[capture][pcapng]") {
  // It carries no timestamp and no interface id. Surfacing it with a fabricated
  // time of zero would silently corrupt any ordering the caller derives.
  file_builder f{true};
  f.section().interface_description().simple_packet("no-time");

  ng::reader r;
  REQUIRE(ng::reader::over(f.view()).get(r) == dfr::error::ok);

  const auto got = drain_all(r);
  CHECK(got.outcome == dfr::error::not_supported);
  CHECK_FALSE(r.done());
}

TEST_CASE("a disagreeing trailing length is refused",
          "[capture][pcapng][regression]") {
  // The repeated length is a free integrity check the format hands us. Ignoring it
  // accepts a corrupted length and then walks to a wrong offset for every block
  // after this one.
  file_builder f{true};
  f.section().interface_description();
  f.block(ng::kEnhancedPacketBlock,
          std::vector<std::uint8_t>(20, 0),
          /*declared_leading=*/32, /*declared_trailing=*/28);

  ng::reader r;
  REQUIRE(ng::reader::over(f.view()).get(r) == dfr::error::ok);

  const auto got = drain_all(r);
  CHECK(got.outcome == dfr::error::message_length_mismatch);
}

TEST_CASE("a length below the framing size is refused",
          "[capture][pcapng][regression]") {
  // A zero or tiny length would either loop forever or land mid-field, so it is
  // validated before it is used to advance.
  for (const long long bad : {0LL, 4LL, 8LL, 11LL}) {
    file_builder f{true};
    f.section().interface_description();
    f.block(ng::kEnhancedPacketBlock, std::vector<std::uint8_t>(20, 0), bad, bad);

    ng::reader r;
    REQUIRE(ng::reader::over(f.view()).get(r) == dfr::error::ok);
    const auto got = drain_all(r);
    CHECK(got.outcome == dfr::error::message_length_mismatch);
  }
}

TEST_CASE("a length that is not a multiple of four is refused",
          "[capture][pcapng]") {
  file_builder f{true};
  f.section().interface_description();
  f.block(ng::kEnhancedPacketBlock, std::vector<std::uint8_t>(20, 0), 33, 33);

  ng::reader r;
  REQUIRE(ng::reader::over(f.view()).get(r) == dfr::error::ok);
  CHECK(drain_all(r).outcome == dfr::error::message_length_mismatch);
}

TEST_CASE("a captured length above the wire length is refused",
          "[capture][pcapng]") {
  file_builder f{true};
  f.section().interface_description().packet(0, "abc", 0,
                                            /*declared_captured=*/3,
                                            /*declared_original=*/2);

  ng::reader r;
  REQUIRE(ng::reader::over(f.view()).get(r) == dfr::error::ok);
  CHECK(drain_all(r).outcome == dfr::error::message_length_mismatch);
}

TEST_CASE("packet data padding is not part of the frame",
          "[capture][pcapng][regression]") {
  // The format pads packet data up to a four-byte boundary. Handing the padding
  // to a protocol decoder makes it report trailing bytes nobody sent.
  file_builder f{true};
  f.section().interface_description().packet(0, "ab");  // 2 bytes, padded to 4

  ng::reader r;
  REQUIRE(ng::reader::over(f.view()).get(r) == dfr::error::ok);

  dfr::capture::frame frame;
  REQUIRE(r.next().get(frame) == dfr::error::ok);
  CHECK(frame.data.size() == 2);
  CHECK(as_text(frame.data) == "ab");
  CHECK_FALSE(frame.truncated());
}

TEST_CASE("a truncated file leaves the reader not done",
          "[capture][pcapng][regression]") {
  // Same invariant as the classic reader: a failed next() consumes nothing, so
  // done() means every byte belonged to a complete block. Without it, a capture
  // cut short by a full disk is indistinguishable from a clean one.
  const auto full = minimal("payload");

  for (std::size_t cut = 1; cut < full.size(); ++cut) {
    const auto partial = minimal("payload").truncate_to(cut);
    ng::reader r;
    if (ng::reader::over(partial.view()).get(r) != dfr::error::ok) {
      continue;  // the section header itself was cut, reported at open
    }
    const auto got = drain_all(r);
    if (got.outcome == dfr::error::ok) {
      CHECK(r.done());
      CHECK(r.remaining() == 0);
    } else {
      CHECK_FALSE(r.done());
      CHECK(r.remaining() > 0);
    }
  }

  ng::reader whole;
  REQUIRE(ng::reader::over(full.view()).get(whole) == dfr::error::ok);
  CHECK(drain_all(whole).outcome == dfr::error::ok);
  CHECK(whole.done());
}

TEST_CASE("interface metadata is exposed", "[capture][pcapng]") {
  file_builder f{true};
  f.section().interface_description(/*link=*/1, /*snaplen=*/1514, /*tsresol=*/9);
  f.packet(0, "x");

  ng::reader r;
  REQUIRE(ng::reader::over(f.view()).get(r) == dfr::error::ok);
  static_cast<void>(drain_all(r));

  const auto* iface = r.interface_at(0);
  REQUIRE(iface != nullptr);
  CHECK(iface->is_ethernet());
  CHECK(iface->snaplen == 1514);
  CHECK(iface->resolution == ng::tick_resolution{9});
  CHECK(r.interface_at(1) == nullptr);
}

TEST_CASE("a file of metadata only ends cleanly", "[capture][pcapng]") {
  // drain() must treat "no more packets" as a clean end rather than an error, or
  // a file whose last blocks are all metadata reports a spurious failure.
  file_builder f{true};
  f.section().interface_description().unknown_block();

  ng::reader r;
  REQUIRE(ng::reader::over(f.view()).get(r) == dfr::error::ok);
  const auto got = drain_all(r);
  CHECK(got.outcome == dfr::error::ok);
  CHECK(got.payloads.empty());
  CHECK(r.done());
}

TEST_CASE("next() on an exhausted reader is a programmer error",
          "[capture][pcapng]") {
  DFR_CHECK_ABORTS({
    file_builder f{true};
    f.section();
    ng::reader r;
    static_cast<void>(ng::reader::over(f.view()).get(r));
    static_cast<void>(r.next());
  });
}
