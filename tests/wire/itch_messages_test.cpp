#include <dfr/wire/itch.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace itch = dfr::wire::itch;

namespace {

constexpr itch::header_fields kHeader{
    .timestamp_ns = 0x010203040506ULL,
    .stock_locate = 0x1234,
    .tracking_number = 0x5678};

}  // namespace

TEST_CASE("ITCH add order matches the published wire layout", "[wire][itch]") {
  std::array<std::byte, 64> bytes{};
  std::size_t written = 0;
  REQUIRE(itch::encode_add_order(dfr::mutable_packet_view{bytes.data(), bytes.size()}, kHeader,
                                 0x0102030405060708ULL, 'B', 125, "NVDA", 905'000)
              .get(written) == dfr::error::ok);
  REQUIRE(written == 36);

  CHECK(bytes[0] == std::byte{'A'});
  CHECK(bytes[1] == std::byte{0x12});
  CHECK(bytes[2] == std::byte{0x34});
  CHECK(bytes[5] == std::byte{0x01});
  CHECK(bytes[10] == std::byte{0x06});
  CHECK(bytes[11] == std::byte{0x01});
  CHECK(bytes[18] == std::byte{0x08});
  CHECK(bytes[19] == std::byte{'B'});
  CHECK(bytes[23] == std::byte{125});
  CHECK(bytes[24] == std::byte{'N'});
  CHECK(bytes[28] == std::byte{' '});

  itch::add_order decoded;
  REQUIRE(itch::decode_add_order(dfr::packet_view{bytes.data(), written}).get(decoded) ==
          dfr::error::ok);
  CHECK(decoded.head.timestamp_ns == kHeader.timestamp_ns);
  CHECK(decoded.head.stock_locate == kHeader.stock_locate);
  CHECK(decoded.order_reference == 0x0102030405060708ULL);
  CHECK(decoded.shares == 125);
  CHECK(decoded.price == 905'000);
  CHECK(decoded.buy);
  CHECK(decoded.symbol == itch::stock{'N', 'V', 'D', 'A', ' ', ' ', ' ', ' '});
}

TEST_CASE("ITCH execution round-trips its order and match identities", "[wire][itch]") {
  std::array<std::byte, 64> bytes{};
  std::size_t written = 0;
  REQUIRE(itch::encode_order_executed(
              dfr::mutable_packet_view{bytes.data(), bytes.size()}, kHeader, 71, 40, 9001)
              .get(written) == dfr::error::ok);
  itch::order_executed decoded;
  REQUIRE(itch::decode_order_executed(dfr::packet_view{bytes.data(), written}).get(decoded) ==
          dfr::error::ok);
  CHECK(decoded.order_reference == 71);
  CHECK(decoded.shares == 40);
  CHECK(decoded.match_number == 9001);
}

TEST_CASE("ITCH cancel delete and replace round-trip", "[wire][itch]") {
  std::array<std::byte, 64> bytes{};
  std::size_t written = 0;

  REQUIRE(itch::encode_order_cancel(
              dfr::mutable_packet_view{bytes.data(), bytes.size()}, kHeader, 10, 25)
              .get(written) == dfr::error::ok);
  itch::order_cancel canceled;
  REQUIRE(itch::decode_order_cancel(dfr::packet_view{bytes.data(), written}).get(canceled) ==
          dfr::error::ok);
  CHECK(canceled.order_reference == 10);
  CHECK(canceled.shares == 25);

  REQUIRE(itch::encode_order_delete(
              dfr::mutable_packet_view{bytes.data(), bytes.size()}, kHeader, 11)
              .get(written) == dfr::error::ok);
  itch::order_delete deleted;
  REQUIRE(itch::decode_order_delete(dfr::packet_view{bytes.data(), written}).get(deleted) ==
          dfr::error::ok);
  CHECK(deleted.order_reference == 11);

  REQUIRE(itch::encode_order_replace(dfr::mutable_packet_view{bytes.data(), bytes.size()},
                                     kHeader, 12, 99, 300,
                                     1'250'000)
              .get(written) == dfr::error::ok);
  itch::order_replace replaced;
  REQUIRE(itch::decode_order_replace(dfr::packet_view{bytes.data(), written}).get(replaced) ==
          dfr::error::ok);
  CHECK(replaced.original_reference == 12);
  CHECK(replaced.new_reference == 99);
  CHECK(replaced.shares == 300);
  CHECK(replaced.price == 1'250'000);
}

TEST_CASE("ITCH decoder refuses unknown types wrong lengths and invalid sides", "[wire][itch]") {
  std::array<std::byte, 64> bytes{};
  bytes[0] = std::byte{'?'};
  CHECK(itch::decode_header(dfr::packet_view{bytes.data(), 36}).error_code() ==
        dfr::error::unknown_message_type);

  bytes[0] = std::byte{'A'};
  CHECK(itch::decode_header(dfr::packet_view{bytes.data(), 35}).error_code() ==
        dfr::error::message_length_mismatch);

  std::size_t written = 0;
  CHECK(itch::encode_add_order(dfr::mutable_packet_view{bytes.data(), bytes.size()},
                               kHeader, 1, 'X', 10, "A", 1)
            .error_code() == dfr::error::invalid_argument);
  REQUIRE(itch::encode_add_order(dfr::mutable_packet_view{bytes.data(), bytes.size()},
                                 kHeader, 1, 'S', 10, "A", 1)
              .get(written) == dfr::error::ok);
  bytes[itch::kSideOffset] = std::byte{'X'};
  CHECK(itch::decode_add_order(dfr::packet_view{bytes.data(), written}).error_code() ==
        dfr::error::invalid_argument);
}

TEST_CASE("ITCH timestamp is bounded to its six-byte field", "[wire][itch]") {
  std::array<std::byte, 64> bytes{};
  auto fields = kHeader;
  fields.timestamp_ns = std::uint64_t{1} << 48;
  CHECK(itch::encode_order_delete(dfr::mutable_packet_view{bytes.data(), bytes.size()},
                                  fields, 1).error_code() ==
        dfr::error::invalid_argument);
}
