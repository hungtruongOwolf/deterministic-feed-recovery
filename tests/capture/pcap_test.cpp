#include <dfr/capture/pcap.hpp>

#include "capture/support/pcap_file.hpp"
#include "support/death_test.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace pcap = dfr::capture::pcap;
using dfr_test::pcap::as_text;
using dfr_test::pcap::file_builder;

TEST_CASE("all four magics are recognised, and each settles two questions",
          "[capture][pcap]") {
  // The magic is 0xa1b2c3d4 written in the writer's byte order, with a variant
  // spelling for nanosecond timestamps. So the four byte sequences encode byte
  // order and timestamp resolution together, and a reader that only checks two
  // of them silently mis-scales every timestamp in the other two.
  struct expectation {
    std::array<std::uint8_t, 4> magic;
    pcap::byte_order order;
    pcap::timestamp_resolution resolution;
  };
  const std::array<expectation, 4> cases{{
      {pcap::kMagicLittleMicros, pcap::byte_order::little,
       pcap::timestamp_resolution::microseconds},
      {pcap::kMagicBigMicros, pcap::byte_order::big,
       pcap::timestamp_resolution::microseconds},
      {pcap::kMagicLittleNanos, pcap::byte_order::little,
       pcap::timestamp_resolution::nanoseconds},
      {pcap::kMagicBigNanos, pcap::byte_order::big,
       pcap::timestamp_resolution::nanoseconds},
  }};

  for (const auto& expected : cases) {
    const file_builder f{expected.magic};
    pcap::reader r;
    REQUIRE(pcap::reader::over(f.view()).get(r) == dfr::error::ok);
    CHECK(r.info().order == expected.order);
    CHECK(r.info().resolution == expected.resolution);
  }
}

TEST_CASE("the file header is decoded", "[capture][pcap]") {
  const file_builder f{pcap::kMagicLittleMicros, /*snaplen=*/1514, /*link=*/1};

  pcap::reader r;
  REQUIRE(pcap::reader::over(f.view()).get(r) == dfr::error::ok);

  CHECK(r.info().version_major == 2);
  CHECK(r.info().version_minor == 4);
  CHECK(r.info().snaplen == 1514);
  CHECK(r.info().is_ethernet());
  CHECK(r.done());  // header only, no records
  CHECK(r.records_read() == 0);
}

TEST_CASE("a pcapng file is reported as unsupported, not malformed",
          "[capture][pcap]") {
  // A file beginning 0a 0d 0d 0a is a pcapng Section Header Block. Saying
  // not_supported rather than a framing error is what lets a caller hand the
  // same bytes to the other reader, which matters because IEX HIST switched
  // format mid-2017 and a tool must handle both.
  const std::array<std::uint8_t, 24> pcapng_start{0x0A, 0x0D, 0x0D, 0x0A};
  const auto opened = pcap::reader::over(
      dfr::packet_view{pcapng_start.data(), pcapng_start.size()});

  CHECK_FALSE(opened.has_value());
  CHECK(opened.error_code() == dfr::error::not_supported);
}

TEST_CASE("a file shorter than its header is refused", "[capture][pcap]") {
  const file_builder f;
  for (std::size_t cut = 0; cut < pcap::kFileHeaderSize; ++cut) {
    const auto partial = file_builder{}.truncate_to(cut);
    const auto opened = pcap::reader::over(partial.view());
    CHECK_FALSE(opened.has_value());
  }
  CHECK(pcap::reader::over(f.view()).has_value());
}

TEST_CASE("records are read in order with their payloads",
          "[capture][pcap]") {
  const auto f = file_builder{}
                     .record(1000, 500, "first")
                     .record(1000, 700, "second")
                     .record(1001, 0, "third");

  pcap::reader r;
  REQUIRE(pcap::reader::over(f.view()).get(r) == dfr::error::ok);

  std::vector<std::string_view> payloads;
  std::vector<std::uint64_t> stamps;
  REQUIRE(r.drain([&](const dfr::capture::frame& fr) {
                payloads.push_back(as_text(fr.data));
                stamps.push_back(fr.timestamp_ns);
              })
              .has_value());

  CHECK(payloads == std::vector<std::string_view>{"first", "second", "third"});
  CHECK(stamps == std::vector<std::uint64_t>{1'000'000'500'000ULL,
                                             1'000'000'700'000ULL,
                                             1'001'000'000'000ULL});
  CHECK(r.records_read() == 3);
  CHECK(r.done());
}

TEST_CASE("the fraction field is scaled by the magic",
          "[capture][pcap][regression]") {
  // 500 microseconds and 500 nanoseconds are the same bytes with different
  // magics. A reader that assumes microseconds is off by a thousand on every
  // nanosecond file, which is invisible until two captures are compared.
  const auto micros = file_builder{pcap::kMagicLittleMicros}.record(0, 500, "x");
  const auto nanos = file_builder{pcap::kMagicLittleNanos}.record(0, 500, "x");

  pcap::reader rm;
  REQUIRE(pcap::reader::over(micros.view()).get(rm) == dfr::error::ok);
  dfr::capture::frame fm;
  REQUIRE(rm.next().get(fm) == dfr::error::ok);
  CHECK(fm.timestamp_ns == 500'000);

  pcap::reader rn;
  REQUIRE(pcap::reader::over(nanos.view()).get(rn) == dfr::error::ok);
  dfr::capture::frame fn;
  REQUIRE(rn.next().get(fn) == dfr::error::ok);
  CHECK(fn.timestamp_ns == 500);
}

TEST_CASE("a big-endian file reads the same values as a little-endian one",
          "[capture][pcap]") {
  // The acceptance test for the byte-order branch: the same logical content in
  // both orders must produce identical frames.
  const auto little = file_builder{pcap::kMagicLittleMicros}
                          .record(0x11223344, 0x00000042, "payload");
  const auto big = file_builder{pcap::kMagicBigMicros}
                       .record(0x11223344, 0x00000042, "payload");

  pcap::reader rl;
  pcap::reader rb;
  REQUIRE(pcap::reader::over(little.view()).get(rl) == dfr::error::ok);
  REQUIRE(pcap::reader::over(big.view()).get(rb) == dfr::error::ok);

  dfr::capture::frame fl;
  dfr::capture::frame fb;
  REQUIRE(rl.next().get(fl) == dfr::error::ok);
  REQUIRE(rb.next().get(fb) == dfr::error::ok);

  CHECK(fl.timestamp_ns == fb.timestamp_ns);
  CHECK(fl.wire_length == fb.wire_length);
  CHECK(as_text(fl.data) == as_text(fb.data));
}

TEST_CASE("a truncated frame is reported as truncated", "[capture][pcap]") {
  // wire_length above the stored length is the normal effect of a snaplen, and
  // it must be preserved rather than clamped: it is how a caller knows the
  // decoder is about to see less than the publisher sent.
  const auto f = file_builder{}.record(1, 0, "abc", /*declared_captured=*/3,
                                       /*declared_original=*/1500);

  pcap::reader r;
  REQUIRE(pcap::reader::over(f.view()).get(r) == dfr::error::ok);
  dfr::capture::frame fr;
  REQUIRE(r.next().get(fr) == dfr::error::ok);

  CHECK(fr.data.size() == 3);
  CHECK(fr.wire_length == 1500);
  CHECK(fr.truncated());
}

TEST_CASE("a stored length above the wire length is refused",
          "[capture][pcap][regression]") {
  // Impossible in a well-formed file, and it is the shape a byte-order
  // misdetection takes: read the wrong way round, a 74-byte record becomes
  // 1,241,513,984. Refusing it turns a silent walk off the end of the buffer
  // into one reported error.
  const auto f = file_builder{}.record(1, 0, "abc", /*declared_captured=*/3,
                                       /*declared_original=*/2);

  pcap::reader r;
  REQUIRE(pcap::reader::over(f.view()).get(r) == dfr::error::ok);
  const auto record = r.next();

  CHECK_FALSE(record.has_value());
  CHECK(record.error_code() == dfr::error::message_length_mismatch);
}

TEST_CASE("a captured length beyond the file is refused",
          "[capture][pcap][regression]") {
  const auto f = file_builder{}.record(1, 0, "abc", /*declared_captured=*/9999,
                                       /*declared_original=*/9999);

  pcap::reader r;
  REQUIRE(pcap::reader::over(f.view()).get(r) == dfr::error::ok);
  const auto record = r.next();

  CHECK_FALSE(record.has_value());
  CHECK(record.error_code() == dfr::error::truncated_block);
}

TEST_CASE("a file cut mid-record is not done", "[capture][pcap][regression]") {
  // The distinction that matters for a capture stopped by a full disk: done()
  // means every byte was consumed, not "no more complete records". A reader that
  // conflated them would report a partial file as a clean one.
  const auto full = file_builder{}.record(1, 0, "complete").record(2, 0, "cut");

  for (std::size_t cut = pcap::kFileHeaderSize + 1; cut < full.size(); ++cut) {
    auto partial = file_builder{}.record(1, 0, "complete").record(2, 0, "cut");
    partial.truncate_to(cut);

    pcap::reader r;
    REQUIRE(pcap::reader::over(partial.view()).get(r) == dfr::error::ok);

    int delivered = 0;
    const auto outcome =
        r.drain([&](const dfr::capture::frame&) { ++delivered; });

    // Either it drained cleanly on an exact boundary, or it reported why not
    // and left the incomplete record unconsumed. What must never happen is a
    // clean drain that silently lost a record, or a failure that still looks
    // exhausted: a caller could not then tell a clean end from a capture cut
    // short by a full disk.
    if (outcome.has_value()) {
      CHECK(r.done());
      CHECK(r.remaining() == 0);
    } else {
      CHECK_FALSE(r.done());
      CHECK(r.remaining() > 0);
    }
    // The first record occupies a 16-byte header plus its 8-byte payload, so it
    // only arrives once the cut is past that. Stating the boundary rather than
    // asserting "at least one" keeps the test honest about small cuts.
    const std::size_t first_record_end = pcap::kFileHeaderSize +
                                         pcap::kRecordHeaderSize +
                                         std::string_view{"complete"}.size();
    CHECK(delivered == (cut >= first_record_end ? 1 : 0));
  }
}

TEST_CASE("a record with no payload is legal", "[capture][pcap]") {
  const auto f = file_builder{}.record(5, 5, "");

  pcap::reader r;
  REQUIRE(pcap::reader::over(f.view()).get(r) == dfr::error::ok);
  dfr::capture::frame fr;
  REQUIRE(r.next().get(fr) == dfr::error::ok);

  CHECK(fr.data.empty());
  CHECK_FALSE(fr.truncated());
  CHECK(r.done());
}

TEST_CASE("a non-Ethernet link type is reported, not assumed",
          "[capture][pcap]") {
  // A capture on a Linux cooked socket (113) or raw IP (101) has a different
  // header, and decoding it as Ethernet yields confident nonsense.
  const file_builder cooked{pcap::kMagicLittleMicros, 65535, /*link=*/113};

  pcap::reader r;
  REQUIRE(pcap::reader::over(cooked.view()).get(r) == dfr::error::ok);
  CHECK_FALSE(r.info().is_ethernet());
  CHECK(r.info().link == 113);
}

TEST_CASE("next() on an exhausted reader is a programmer error",
          "[capture][pcap]") {
  DFR_CHECK_ABORTS({
    const file_builder f;
    pcap::reader r;
    static_cast<void>(pcap::reader::over(f.view()).get(r));
    static_cast<void>(r.next());
  });
}
