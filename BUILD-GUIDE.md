# Build Guide: Nguồn dữ liệu, mục tiêu test, và lộ trình học

Nghiên cứu 2026-07-29/30. Mọi URL và số byte dưới đây đã được xác minh bằng `curl` / `gh api` /
phân tích byte trực tiếp. Chỗ nào không xác minh được đều ghi rõ.

---

## PHẦN 1: NGUỒN DỮ LIỆU TEST

### 1.1 Bắt đầu ở đây: IEX HIST

Đây là nguồn tốt nhất, không có gì sánh được, vì 5 lý do đã kiểm chứng trực tiếp:

1. **Là pcap wire-format thật**: Ethernet + 802.1Q VLAN + IPv4 multicast + UDP + IEX-TP. Không
   phải CSV đã chuẩn hoá, không phải file-format dựng lại. **[ĐÃ KIỂM BYTE]**
2. **IEX-TP gần như song sinh với MoldUDP64**: header 40 byte với `Session ID`,
   `First Message Sequence Number`, `Message Count`, `Stream Offset`, `Send Time`. Parse 20.145
   packet → **0 chuyển tiếp sequence bị đứt, 0 lệch stream-offset**. Ground truth chính xác tuyệt
   đối. **[ĐÃ KIỂM BYTE]**
3. **Có spec recovery đầy đủ, đọc tự do**: A/B line arbitration, Gap Fill qua cả UDP và TCP
   unicast với Request Range Blocks, Gap Fill Test Request/Response, heartbeat, session
   termination: cộng thêm giao thức snapshot riêng (**SNAP**). Đây là thứ hiếm nhất trong toàn
   bộ lĩnh vực này: một spec recovery thật, kèm dữ liệu thật khớp với nó.
4. **Miễn phí, không cần key, không đăng ký, không click-through ToS.** Cập nhật T+1.
   **2.434 ngày giao dịch, 2016-12-12 → 2026-07-28.**
5. **Range request hoạt động** → không bao giờ cần tải 13 GB. Prefix 4 MB của gzip stream giải nén
   ra ~17 MB pcapng hợp lệ. **[ĐÃ KIỂM BYTE]**

**Lệnh lấy file test đầu tiên:**

```bash
# 712 KB nén → 3,4 MB pcap. Thứ Bảy 2017-08-26 = phiên cuối tuần:
# nhỏ, nhưng đầy đủ về cấu trúc (session ID thật, sequence number thật, 13.220 heartbeat).
curl -L -o deep_20170826.pcap.gz \
 'https://www.googleapis.com/download/storage/v1/b/iex/o/data%2Ffeeds%2F20170826%2F20170826_IEXTP1_DEEP1.0.pcap.gz?generation=1503943426454167&alt=media'
gunzip deep_20170826.pcap   # → 3.405.890 byte, classic libpcap (d4c3b2a1), µs, Ethernet
```

File này là **classic pcap** (dễ parse hơn pcapng): chọn có chủ ý. Sau đó cho một ngày giao dịch
thật:

```bash
# Luôn resolve lại link: token ?generation= là phần version của object.
curl -s 'https://iextrading.com/api/1.0/hist?date=20191224' | python3 -m json.tool
# 20191224 = phiên nửa ngày: DEEP 171.929.929 B (ngày thường nhỏ nhất), TOPS 160,6 MB
```

**Ba cái bẫy phải biết trước khi viết parser:**

| Bẫy | Chi tiết |
|---|---|
| **Đổi từ pcap sang pcapng giữa đường** | `20170615` = classic pcap (`d4c3b2a1`). Từ `20170620` trở đi = **pcapng** (`0a0d0d0a`). Cần cả hai reader. **[ĐÃ KIỂM BYTE]** |
| **Có tag 802.1Q VLAN** | VLAN 1013 trong file 2017. Nếu hardcode `ethertype @ offset 12 == 0x0800` thì parse ra **số không**. **[ĐÃ KIỂM BYTE]** |
| **HIST pcap chỉ chứa MỘT multicast group** | `233.215.21.4:10378` cho DEEP. IEX-TP *có định nghĩa* A/B line, nhưng bản lưu trữ chỉ capture một line. **Không thể test A/B arbitration bằng dữ liệu IEX HIST.** Dùng B3 cho việc đó. |

### 1.2 Hai nguồn bổ trợ bắt buộc phải có

**a) MoldUDP64 pcap thật của NASDAQ**: giải quyết lỗ hổng lớn nhất của kế hoạch:

`Open-Markets-Initiative/omi-data-pcaps` → `nasdaq/NasdaqEquities/TotalViewItch.v5.0.zst`
(17.728.924 B → 71.697.245 B). Đây là pcap **MoldUDP64 multicast thật** của NASDAQ
TotalView-ITCH 5.0: session `000010059B`, seq 10.312.965 → 10.740.083, **399.999 chuyển tiếp liền
mạch / 0 đứt**, 627.466 packet trên `233.54.12.111:26477`, VLAN 141. **[ĐÃ KIỂM BYTE]**

**b) Cặp A/B thật**: nguồn duy nhất tìm được trên toàn thế giới:

B3 (sàn Brazil): `MBO_EQT_Incremental_FeedA.zip` + `FeedB.zip` (1.422.257.073 + 1.421.936.528 B,
miễn phí, không cần key): **cặp capture A/B dư thừa của cùng một session**, cộng thêm
`MBO_EQT_SnapshotRecovery.zip` (204.135.545 B) riêng. Đây là thứ duy nhất cho phép test A/B
arbitration bằng dữ liệu thật.

### 1.3 ⚠️ PHÁT HIỆN QUAN TRỌNG: dữ liệu NASDAQ miễn phí KHÔNG có transport layer

Đây là phát hiện làm thay đổi kế hoạch, và nó đúng cho **mọi file kể cả file 2026 mới nhất**.

Byte-dissect ba file:

```
01302019.NASDAQ_ITCH50.gz   → 00 0c | 53 ... 4f    (len=12, 'S' System Event, code 'O')
                               00 27 | 52 ...       (len=39, 'R' Stock Directory)
S061226-v50.txt.gz (17,9GB) → 00 0c | 53 ... 4f    framing y hệt
itch50_05_18.gz    (16,1GB) → 00 0c | 53 ... 4f    framing y hệt
```

Giải mã 24 message liên tiếp, `len/type` khớp hoàn hảo với kích thước struct ITCH 5.0
(`S`=12, `R`=39, `A`=36, `F`=40, `U`=35, `X`=23, `E`=31, `P`=44, `D`=19). **[ĐÃ KIỂM BYTE]**

**Đây là định dạng NASDAQ "BinaryFILE": chỉ có 2 byte length big-endian + raw ITCH message. KHÔNG
có header MoldUDP64, KHÔNG có session ID, KHÔNG có packet sequence number, KHÔNG có UDP, KHÔNG có
pcap.** Toàn bộ transport layer đã bị bóc đi.

Xác nhận bằng chính spec của NASDAQ
(`nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/binaryfile.pdf`, 84.384 B):

> *"BinaryFILE is a very simple file format used to deliver a set of sequenced messages inside a
> static file... It is intended as an **off-line companion** for real-time message delivery
> protocols like SoupBinTCP and MoldUDP64."*
> *"A **message of length zero is used to indicate the end of the session.**"*

**Hệ quả:** nếu chỉ dùng `emi.nasdaq.com`, bạn sẽ phải **tự tổng hợp toàn bộ transport layer**,
tự nghĩ ra session ID, tự đóng gói message vào datagram MoldUDP64, tự sinh heartbeat, rồi tự tiêm
gap vào cái framing tự tạo đó. **Gap của bạn sẽ đang test chính generator của bạn, không phải hành
vi wire thật.** → Dùng OMI MoldUDP64 pcap ở §1.2a cho framing thật; dùng emi.nasdaq.com cho độ
thực tế về volume và message-mix.

Ghi thêm: mọi file `.md5sum` trong directory đó đều **404** (IIS thiếu MIME handler). Đừng xây
checksum verification dựa vào chúng.

### 1.4 Crypto: corpus gap-recovery miễn phí và vô hạn

Vì các sàn crypto phát sequence number đơn điệu và **công bố thủ tục resync bằng văn bản**, chúng
cho bạn nguồn test case gap-recovery vô hạn, miễn phí, có ground truth. Binance có quy trình
"how to manage a local order book": về bản chất đó *chính là* một thuật toán gap-recovery, với
các field `U`/`u`. Coinbase Advanced có `sequence_num`. Dùng làm nguồn bổ sung, không phải nguồn
chính (JSON/WebSocket, nên không dạy gì về binary multicast).

---

## PHẦN 2: SPEC: LẤY Ở ĐÂU

Tất cả các link dưới đây **không cần đăng ký** và đã xác minh live:

| Spec | URL |
|---|---|
| MoldUDP64 V1.00 (6 trang, đầy đủ) | `nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/moldudp64.pdf` |
| SoupBinTCP 3.00 (9 trang) | `.../dataproducts/soupbintcp.pdf` |
| **Nasdaq GLIMPSE 5.0 (32 trang)** | `.../dataproducts/nqglimpsespecification.pdf` ⚠️ chú ý tên file: `glimpse.pdf`, `nqglimpse.pdf` đều 404 |
| TotalView-ITCH 5.0 | `.../dataproducts/nqtvitchspecification.pdf` |
| OUCH 4.2 | `.../TradingProducts/ouch4.2.pdf` |
| BinaryFILE | `.../dataproducts/binaryfile.pdf` |
| IEX-TP 1.25 (15 trang) | ⚠️ URL live chỉ trả về stub 1 trang "đã chuyển"; bản thật ở `web.archive.org/web/2020/https://iextrading.com/docs/IEX%20Transport%20Specification.pdf` |
| IEX DEEP 1.x (44 trang) | cùng pattern; có địa chỉ A/B/C thật và giới hạn retransmit |
| UTP Data Feed Services v4.1 | `utpplan.com/DOC/UTPBinaryOutputSpec.pdf` |
| ⭐ **SIAC Retransmission and Snapshot User Guide v1.8** | `cdn.opraplan.com/documents/SIAC_Retransmission_User_Guide.pdf`: **tài liệu công khai tốt nhất về một retransmit facility production thật**: rate limit thật, response code, semantics của sequence reset |
| OPRA Common IP Multicast Distribution Network | `cdn.opraplan.com/documents/OPRA_Common_IP_Multicast_Distribution_Network.pdf`: kiến trúc A/B stream, 156 line → 312 group |
| RFC 3208 (PGM) | `rfc-editor.org/rfc/rfc3208.txt` |

---

## PHẦN 3: DANH SÁCH MỤC TIÊU ĐỂ TEST

Đây là deliverable quan trọng nhất của phần nghiên cứu này. Sau khi sweep **232 repo market-data**
(dedupe từ 30+ query repo + 8 query code-identifier), rồi **đọc source thật** thay vì tin cái tên:
chỉ có **9 implementation trên toàn GitHub** thực sự có state machine gap MoldUDP64 **kèm phát
retransmission request**.

### Tier 1: đã xác minh bằng cách đọc source

| Mục tiêu | ★ | Push | License | Bằng chứng đã đọc |
|---|---|---|---|---|
| **paritytrading/nassau** | 106 | 2026-07-25 | Apache-2.0 | `MoldUDP64Client.java:269-284`, `nextExpectedSequenceNumber`, state `BACKFILL` vs `GAP_FILL`, `requestUntilSequenceNumber`, cold-start (`==0`). 21 test file, CI, CHANGELOG. **Đây là semantics tham chiếu.** |
| **penberg/helix** | 122 | 2017-10-18 | BSD-2 | `moldudp64.hh:93-136`, drop stale/dup, vào `gap_fill`, `retransmit_request()` phát `htobe64(expected_seq_no)`. Yếu: 1 test file, không compile được. |
| **leonardorufino/ll-hft** | 1 | 2026-05-20 | MIT | `receiver_session.hpp` (838 dòng): reorder buffer power-of-two, `try_pop(expected_seq)` validate `slot._first_sequence_number`, `RetransmissionController` riêng với deadline timer. **Thiết kế recovery hoàn chỉnh nhất trong mọi repo gần đây.** 301 file / 55 test file; 90 commit trải **76 ngày khác nhau** (nhịp của người thật). |
| **ohmp7/MarketDataFeedHandler** | 3 | 2026-01-12 | none | `moldudp64.cpp:54-75`, `Backfill (cold start)` / `Gapfill` / update recovery window rõ ràng, có sentinel `kSynchronized` + `std::max`. Port C++ trung thực của nassau. **0 test file.** |
| **anminfang-tamu/astra-feed-engine** | 0 | 2026-07-30 | none | `MoldUdpDecoder.cpp:98-121`, `ChannelHealth::GapDetected` cộng một `heartbeat_next_seq_high_watermark` riêng, để heartbeat đẩy watermark thay vì bị tính thành gap. **Đó là một chi tiết protocol đúng và không hiển nhiên** mà LLM rất ít khi tự nghĩ ra. |
| **Rfkir/HFT_CPP** | 0 | 2025-11-27 | none | `moldUDP64_rxtx.cpp:234-288`, hai đường gap, request bị clamp `std::min<uint64_t>(gap, 60000)` (đúng cap thật của protocol). Comment tiếng Thổ: không phải English của LLM. |
| **JacobNickerson/money-matcher** | 1 | 2026-04-23 | none | `receiverhandler.rs` (436 dòng): `retransmission_socket`/`addr` riêng, `gap_buffer: BTreeMap<u64, MarketEvent>`. |
| **an-thony350/ITCH-Feed-Handler-and-Order-Book** | 3 | 2026-07-29 | MIT | `rtl/mold_seq_guard.sv` (138 dòng) + `mold_deframe.sv` (862 dòng), **kèm** `tb/test_mold_seq_guard.py` cocotb (274 dòng) và 2 xsim testbench (920/216 dòng), cộng `docs/moldudp64_seq_handling.md`. **Mục tiêu gần đây mạnh nhất.** README: 0 emoji, 0 badge, 52 dòng số thật. |
| **jamisonrobey/nasdaq-moldudp64-feed-sim** | 0 | 2026-07-12 | MIT | Phía **server/rewinder**: xem §3.3 |

Ngoài MoldUDP64: **epam/java-cme-mdp3-handler** (82★, LGPL-3.0, 280 file / **129 test file**, chủ
sở hữu là công ty EPAM) là mục tiêu CME MDP3 tốt nhất. **spiretrading/nexus** (4.298 file, 249
test file, 10 năm lịch sử) engineering xuất sắc nhưng `MoldUdp64Client.hpp` chỉ 128 dòng, **không
có token gap nào**: chỉ làm framing, sai layer.

### 3.1 ⭐ `coryan/jaybeams`: negative control miễn phí

`mold_udp_channel.cpp:71-104` phát hiện `sequence_number != expected_sequence_number_` rồi **log
và tiếp tục**. Comment trong source nói thẳng: *"since we are not dealing with gaps, or message
reordering."* Apache-2.0, 432 file, tác giả là engineer của Google.

Đây đúng là con canary mà bạn cần, mà nó đã tồn tại, tự thừa nhận trung thực, và không cần ai đi
tố. **Harness của bạn PHẢI flag được jaybeams, không thì harness của bạn sai.** Đặt nó vào CI như
một fixture bắt buộc-phải-fail ngay ngày đầu. Nó cũng cho bạn một cách công bố kết quả "không
recover được" một cách tử tế: tác giả đã tự ghi rõ giới hạn phạm vi.

Control phụ: `evanap003300/orderbook` (*"here we log and skip"*), `ArkaKhorchidian` (`detect_gap()`
trống, không recovery).

### 3.2 ⚠️ Bẫy chết người: hai "implementation độc lập" thực ra là cùng một code

`nixiz/itch-bist-parser`: file `moldudp64.hh` **149 dòng, so với 147 dòng của `penberg/helix`,
identifier giống nhau ở gần đúng số dòng**. Đây là fork, không phải implementation độc lập.

Đây là bẫy nguy hiểm nhất cho consensus oracle. Hai fork trong một cuộc bầu 3 phiếu sẽ tạo ra
**đa số 2-1 mà thực chất chỉ là cùng một bug upstream**, và nó sẽ khiến implementation thứ ba
(đúng) trông như đang sai. Trước khi nhận bất kỳ implementation nào vào consensus set, phải diff
nó với mọi thành viên khác và yêu cầu độc lập thật. `helix` và `nixiz` tính là **một phiếu**.

**Consensus set đã xác minh độc lập:** `nassau` (Java) + `helix`-hoặc-`nixiz` (một phiếu, C++) +
`ll-hft` (C++) + `astra-feed-engine` (C++) + `std::map` referee của chính bạn = **4 phiếu độc lập
thật cộng một model**: đủ cho phán quyết 3-2 mà không cần người phân xử.

### 3.3 Có người đã xây sẵn *nguồn* fault injection cho bạn

`jamisonrobey/nasdaq-moldudp64-feed-sim` (MIT, 56 file, 17 test file, 2 CI workflow) là một
**server** MoldUDP64 với `imr/mold/retransmission/{feed,feed_pool}` và
`downstream/{feed,heartbeat,pacer}`. Tức là một transmitter có rewinder và có bộ trả lời
retransmission.

Điều này giải quyết một nửa việc: **test gap *recovery* là bất khả thi nếu không có gì trả lời
retransmit request.** Không ai recover được từ gap nếu không có ai đáp. Đánh giá repo này trước
khi tự viết transmitter.

Hai món quà nữa: `joaquinbejar/itch-rs` có **13 fuzz target**, và `bbalouki/itchcpp` có
`fuzz/moldudp64_fuzzer.cpp`. Thu hoạch corpus của chúng làm seed bất kể có test repo đó hay không.

### 3.4 ⚠️ Làn sóng repo 2026: đã lượng hoá

232 repo market-data sau dedupe. **156 (67%) được tạo trong 19 tháng gần nhất**: 51 trong 2025,
105 trong 7 tháng đầu 2026, so với baseline 2020–2024 là ~7/năm. Tức là tăng khoảng **25×**.

Trong 156 repo đó: **82% có 0 sao**, 80% không license, 58% C++, **21% chỉ có 1 commit**, **49% có
toàn bộ commit trong đúng một ngày**, 71% có ≤3 ngày commit khác nhau, **58% được tạo và push lần
cuối trong cùng một ngày**. Tuổi thọ trung vị: **1 ngày**. Bảy repo là **git repo hoàn toàn rỗng**.

**Tỉ lệ dùng được của cả làn sóng: ~12–15 trên 156 (~9%).**

Quan trọng: **151 owner khác nhau trên 156 repo**, đây không phải bot farm, mà là ~150 cá nhân
mỗi người tạo một artifact portfolio dùng một lần. Đó là dấu hiệu của CV có LLM hỗ trợ, không phải
spam.

**Một sub-pattern cụ thể nên nêu trong README của bạn:** 55 repo gần đây trùng tên
`MarketDataFeedHandler`, ba cái tên thẳng là `qode-c-assignment-market-data-feed-handler` /
`C-_Assignment_Qode_MarketDataFeedHandler` / `Market-Data-Feed-Handler-Assignment`, và **bảy cái
rỗng**. Đây là *một* bài take-home tuyển dụng bị nhân bản ~55 lần. **Loại toàn bộ**: test nó sẽ
trông như đang farm số lượng.

Minh hoạ tốt nhất vì sao khớp tên file là vô dụng: **`ray-27/ITCH-feedhandler` có đúng 1 commit,
không README, không license, nhưng chứa `gap_fill_client.hpp` với một API retransmit trông rất
hợp lý.**

### 3.5 Không có gap handling dù tên gợi ý có

`th2-net/th2-codec-moldudp64`, `bbalouki/itchcpp` (transport không làm gì, dù có mold fuzzer),
`sqfzy/ephemeral` (208 dòng, 0 token), `csinitiative/fhce`, `vladium/vrt`, `ikravets/ev`,
`Mister-Meeseeks/moldudp-unwrap`, `Francklin9999/low-latency-trading-pipeline` (header 19 dòng).

**Overclaim tên: không tồn tại source như vậy:** `Kirill-Katz/itch-ingestion-engine` và
`Yashwanth-1412/QuantLink` đều xuất hiện trong search MoldUDP64; tree walk đệ quy toàn bộ tìm
được **0 file** khớp `mold|udp|seq|recv`. Lời khẳng định chỉ tồn tại trong văn bản README.

---

## PHẦN 4: BÀI HỌC KỸ THUẬT QUAN TRỌNG NHẤT: RACE CONDITION CỦA GLIMPSE

Đây là trái tim của thiết kế, và là thứ fault injector nên tấn công.

```
t0   Bạn phát hiện gap không recover được (hoặc đang start lạnh).
t1   Bạn mở kết nối SoupBinTCP tới Glimpse và login với seq=1.
     ---- TRONG KHI ĐÓ, multicast ITCH vẫn chạy full rate ----
t2   Glimpse bắt đầu "spin". Đây là snapshot của trạng thái TẠI THỜI ĐIỂM login request (t1).
t3   ... spin tiếp tục. NASDAQ có ~8-11k symbol, full displayable book cỡ 10^5-10^6 order đang
     nằm chờ. Spin là đúng số Add Order đó qua MỘT kết nối TCP. Thực tế: hàng chục giây.
t4   Bạn nhận "G" với Sequence Number = N (seq của ITCH tại t1).
t5   Bạn giờ phải ở đúng vị trí để xử lý ITCH message số N.
```

**Race nằm ở đây: `N` được xác định tại `t2`, nhưng bạn chỉ biết nó tại `t4`.** Mọi thứ multicast
phát ra trong `[t2, t4]`: có thể là hàng chục giây dữ liệu full rate, hàng trăm nghìn message,
vừa **cần thiết** vừa **chưa dùng được**, vì bạn không thể apply trước khi snapshot load xong, mà
bạn không thể biết bắt đầu từ đâu trước khi snapshot kết thúc.

**Kiến trúc đúng duy nhất:**

```
Khi vào state RECOVERING_VIA_SNAPSHOT:
  1. GIỮ socket multicast đã join và TIẾP TỤC drain nó. Đừng đóng.
     Buffer mọi message kèm sequence number vào một ring có giới hạn.
     (Nếu đóng socket để "đỡ việc", bạn đã biến một gap recover được thành
      không recover được, vì retention window của re-request server là hữu hạn
      và bạn sẽ rơi khỏi đuôi nó.)
  2. Song song, chạy Glimpse spin vào một book BÓNG (shadow). Không bao giờ chạm book live.
  3. Khi nhận "G" với sequence N:
       - bỏ message đã buffer có seq <  N
       - apply message đã buffer có seq >= N vào shadow book, theo thứ tự
       - nếu seq thấp nhất còn giữ trong buffer > N  ==>  RECOVERY THẤT BẠI.
         Bạn buffer quá muộn hoặc quá ít. Escalate: re-request [N, buffer_low)
         từ Re-request Server, hoặc restart toàn bộ snapshot.
       - khi đã bắt kịp live, swap shadow -> live một cách atomic
  4. Chỉ đến lúc này mới publish.
```

**Vì sao đây là chỗ đúng để fault injector tấn công**: mọi failure mode đều **âm thầm** và đều
tạo ra một book **trông hợp lý nhưng sai**:

| Fault tiêm vào | Client đúng phải làm gì | Triệu chứng của client sai |
|---|---|---|
| Buffer multicast nhỏ hơn (thời gian spin × peak rate) | Phát hiện `buffer_low > N`, escalate | Âm thầm apply từ `buffer_low`, **mất vĩnh viễn** `[N, buffer_low)`. Book có order ma mãi mãi. |
| Spin chậm (throttle TCP của Glimpse xuống 1 Mbps) | Buffer có giới hạn đầy → phát hiện → escalate hoặc restart | OOM, hoặc ring wrap và ghi đè mà không ai biết |

---

## PHẦN 5: THIẾT KẾ FAULT INJECTOR

**Nguyên tắc số một: mô hình hoá theo BURST, không theo xác suất.** Một injector kiểu
`drop_probability=0.001` gần như không test được gì thật.

Tham số hoá đúng:

```
BurstLoss   { start_offset, duration_ms ∈ [1,10], rate_multiplier ∈ [3,10],
              lines: {A}|{B}|{A,B}|all, channels: one|correlated_subset|all }
Reorder     { seq, displacement, delay_ns ∈ [1µs, 10ms] }
Duplicate   { seq, whole | straddling_expected }
LineDeath   { line, mode: silent | stale_seq | heartbeat_only | lagging(growing) }
Epoch       { session_change | seq_rollover_2^32 | reset_to_1 ± marker
              | sequence_discontinuity }
Heartbeat   { stop_data_keep_hb | stop_both | hb_next_expected_ahead | freeze_hb_seq }
Facility    { partial_response(n) | no_response | reject(code)
              | ignore_second_concurrent | terminate_on_malformed
              | rate_limit_then_dos(60s) }
Snapshot    { slow_spin | splice_behind_buffer | splice_ahead | transact_time_mismatch
              | join_mid_loop | totnumreports_grows | session_changes_mid_spin
              | gap_within_buffered_stream | connection_drops_mid_spin }
Event       { drop_end_of_event | empty_end_of_event | split_across_n_packets
              | splice_mid_event }
Framing     { exceed_mtu | drop_fragment | unknown_msg_type | msg_longer_than_expected
              | split_tcp_at_every_byte | coalesce_many_packets_per_recv }
Consumer    { stall_ns, egress_backpressure }
StateReset  { quote_wipeout | per_security_wipe_no_message | channel_reset }
```

**Tính tất định:** seed mọi thứ từ một PRNG seed duy nhất; làm injector thành một hàm tất định của
`(seed, packet_index)` để case fail replay chính xác. Lấy thời gian từ một clock được inject vào,
để test được backoff timer mà không phải chờ thật.

**Injector giá trị cao nhất, theo thứ tự:** `Facility.reject` (verify không có retry storm và
không tự DoS mình) → `Snapshot` (cả 8 dòng) → `BurstLoss` tương quan trên mọi line và channel →
`LineDeath.silent` một line → `Epoch` → `Event` (tính nguyên tử của event).

**Và hãy tiêm lỗi vào chính đường recovery.** Nguyên nhân gốc sự cố NASDAQ 22-08-2013 là một đường
failover có bug tiềm ẩn **chưa từng được chạy thật**. Code recovery của bạn là code ít được test
nhất, và nó chỉ chạy khi mọi thứ đã tệ. Dùng ý tưởng **Gap Fill Test Request** của IEX, tổng quát
hoá: chạy đường recovery liên tục trong production với một gap tổng hợp, để nó không bao giờ lạnh.

---

## PHẦN 6: LỘ TRÌNH HỌC (đã cắt gọn)

**Đảo thứ tự quan trọng nhất:** dự án này **đổi nanosecond lấy tính đúng đắn**, nên memory model
là yêu cầu *vệ sinh* (một SPSC handoff đúng, một seqlock), **không phải** điểm khác biệt. Điểm
khác biệt là protocol semantics, lý thuyết ordering/recovery, và fault injection tất định.

Tổng ~60 giờ, học đúng lúc cần, không học trước hàng loạt:

| # | Lĩnh vực | Giờ | Vì sao ở vị trí này |
|---|---|---|---|
| 1 | Domain trading: **chỉ một lát mỏng** | 10 | Không thể thiết kế recovery layer trước khi biết MoldUDP64 sequencing và Glimpse làm gì. Chặn mọi thứ. |
| 2 | Nền tảng DST (2 talk) | 4 | Quyết định **kiến trúc** của cả 3 thành phần. Xem trong tuần đầu. |
| 3 | Distributed systems: chọn lọc | 8 | Cung cấp vốn từ mà write-up phải dùng để đáng tin. |
| 4 | Linux multicast networking | 10 | Cần cho receiver + harness test cục bộ. |
| 5 | Memory model / lock-free | 12 | Cần, nhưng **ít hơn** cảm giác ban đầu. |
| 6 | C++20 coroutines | 10 | **Có thể hoãn.** Xem cảnh báo dưới. |

### Top 5 phải xem (tổng 4h13m)

| # | Talk | ID YouTube | Thời lượng | Vì sao |
|---|---|---|---|---|
| 1 | **Will Wilson: Testing Distributed Systems w/ Deterministic Simulation** | `4fFDFbi3toc` | 40:20 | Tuyên ngôn của dự án bạn, từ FoundationDB. Xem đầu tiên. |
| 2 | **TigerBeetle: How to write your own Deterministic Simulator** | `JoYjji1DZCE` | 71:22 | **Tài nguyên hành động được nhất trong toàn bộ danh sách**, người ta xây đúng Thành phần 3 của bạn. |
| 3 | **Carl Cook: When a Microsecond Is an Eternity** (CppCon 2017) | `NH1Tta7purM` | 60:07 | Talk cho bối cảnh domain. |
| 4 | **Gil Tene: How NOT to Measure Latency** | `lJ8ydIuPFeU` | 42:59 | Tiêm vaccine chống cách #1 khiến số liệu công bố bị phá. Xem **trước khi** công bố bất cứ gì. |
| 5 | **David Gross: When Nanoseconds Matter** (CppCon 2024) | `sX2nF1fW7kI` | 88:51 | Cách một engineer trading firm thật nói về chuyện này. |

### Tài liệu đọc, theo thứ tự ưu tiên

1. **Databento microstructure guide**: `databento.com/microstructure` + `/market-data-feeds`.
   Tài nguyên miễn phí hiện đại tốt nhất và đúng chủ đề: MBP vs MBO, L1/L2/L3, các loại feed.
   **ROI cao hơn Harris cho dự án này.** (4h)
2. **Chính các spec ở Phần 2**: đây là tài liệu học thật. Dự án của bạn *chính là* một
   implementation của những semantics này. Đọc phần sequencing và recovery đến khi đọc thuộc.
3. **Kleppmann, Cambridge "Concurrent and Distributed Systems"**:
   `cl.cam.ac.uk/teaching/2223/ConcDisSys/dist-sys-notes.pdf`, **chỉ Ch 2, 3, 4**. Ch 4
   "Broadcast protocols and logical time" *chính là* lý thuyết của một multicast feed có gap-fill
   và A/B arbitration. Ch 2 cho vốn từ về fault model. (6h)
4. **Phi accrual failure detector**:
   `dspace.jaist.ac.jp/dspace/bitstream/10119/4784/1/IS-RR-2004-010.pdf`, §2–4. Đúng bài toán
   **line chết vs line chậm**. Timeout ngây thơ là câu trả lời sai hiển nhiên và bài này giải
   thích vì sao. 17 trang. (1.5h)
5. **Lamport, "Time, Clocks, and the Ordering of Events"**: 8 trang, và nó là nền tảng của
   "sequence number *chính là* logical time". (1h)
6. **Martin Fowler, LMAX architecture**: `martinfowler.com/articles/lmax.html`. Kiến trúc LMAX
   **chính là** pattern của bạn: ring sequenced single-writer, log event-sourced, business logic
   đơn luồng tất định **để có thể replay**. Cùng ý tưởng với DST, từ một sàn thật. (2h)
7. **Preshing**: `preshing.com`, đọc theo thứ tự: `an-introduction-to-lock-free-programming` →
   `memory-reordering-caught-in-the-act` (**bài "aha"**) → `memory-barriers-are-like-source-control-operations`
   → `acquire-and-release-semantics` → `the-synchronizes-with-relation`. **Pedagogy tốt nhất
   trong lĩnh vực, và miễn phí.** (4h)
8. **Hans Boehm: "Using weakly ordered C++ atomics correctly"** (CppCon 2016, `M15UKpNlpeM`,
   63:25). Giá trị/phút cao nhất. **Ưu tiên hơn Sutter nếu thiếu thời gian**, và nó chống lưng
   trực tiếp cho luận điểm TSan của bạn.
9. **Russ Cox, memory models**: `research.swtch.com/hwmm` rồi `/plmm`. Giải thích x86-TSO vs
   ARM/POWER tốt nhất ở bất cứ đâu. **Chống lưng cho pitch**: "TSan trên x86 che đi bug sẽ lộ
   trên Graviton."
10. **Zeller & Hildebrandt, "Simplifying and Isolating Failure-Inducing Input"** (TSE 2002):
    `st.cs.uni-saarland.de/papers/tse2002/tse2002.pdf`. Bài **ddmin** kinh điển, và là **thứ nhân
    giá trị artifact của bạn nhiều nhất**. "Reproduce được từ seed 4711" là tốt. "Đã rút gọn
    xuống counterexample tối thiểu 3 packet, và đây là invariant bị vi phạm" thì *mạnh hơn nhiều*.
11. **Jepsen analyses**: `jepsen.io/analyses`. Đọc 1–2 bài **để học cách viết, không phải kỹ
    thuật**. Đây là chuẩn vàng cho "đây là bug, đây là cách reproduce, đây là giới hạn của việc
    tôi test".

### ⚠️ Cắt bỏ, kèm lý do

- **MIT 6.824** (giờ là **6.5840**): **phần lớn sai hướng.** Xương sống của nó là consensus và
  replication (MapReduce, GFS, Paxos, Raft ×2, ZooKeeper, Spanner, BFT, Bitcoin). **Không có bài
  nào về failure detection, không có bài nào về Lamport clocks.** Dự án của bạn **không có bài
  toán consensus**. Chỉ nên xem **2 trong 22 bài**: LEC 8 (Consistency & Linearizability) và
  LEC 15 (IronFleet). **Bỏ hoàn toàn phần lab.**
- **Raft**: đừng implement; không có leader election trong một recovery layer. Chỉ đọc **§5.3
  (log matching)** và **§7 (snapshotting)** như analogy thiết kế.
- **Bouchaud et al "Trades, Quotes and Prices"**: cắt hoàn toàn. Dành cho quant *researcher*.
- **Almgren**: cắt. Optimal execution, sai ngành.
- **Harris "Trading and Exchanges"**: **chỉ Ch 4, 5, 6** (~120 trang).
- **Daniel Anderson, `atomic<shared_ptr>`** (CppCon 2024): **cắt.** Nó giải quyết *memory
  reclamation*. Thiết kế của bạn dùng ring buffer có giới hạn và arena/slab, **không nên có
  `shared_ptr` nào gần hot path.**
- **Fedor Pikus, "The Art of Writing Efficient Programs"**: cắt (sách *performance*; đây không
  phải dự án performance).
- **McKenney "Is Parallel Programming Hard"**: **chỉ Ch 15 (Memory Ordering) và Ch 11
  (Validation)**. Đừng đọc hết 700+ trang.
- **Elle**: cắt *kỹ thuật* (nó tìm cycle trong dependency graph của transaction; bạn không có
  transaction). Đọc *báo cáo* Jepsen, không phải Elle.
- **`memory_order_consume`**: bỏ. Compiler promote nó thành acquire.

### ⚠️ Cảnh báo về coroutine trước khi đầu tư 10 giờ

Bản thân **scheduler tất định là phần dễ**: một priority queue theo thời gian mô phỏng cộng một
PRNG có seed. Coroutine chỉ làm *code người dùng* dễ viết hơn. Bằng chứng: **VOPR của TigerBeetle
không dùng coroutine** (state machine tường minh + event loop tất định), và Flow của FoundationDB
dùng CPS/actor được compile.

Luận điểm "C++20 coroutine xoá bỏ rào cản từng khiến Flow thành một dialect riêng" là đúng và
**đó chính là điểm khác biệt**. Nhưng: **hãy xây core simulator bằng event queue tường minh trước
và cho nó pass test, rồi chỉ với tới coroutine khi ergonomics của state machine viết tay thực sự
bắt đầu đau.** Đừng để lý thuyết coroutine trở thành cái cớ trì hoãn.

Khi cần: **Lewis Baker** ở `lewissbaker.github.io`, theo thứ tự `coroutine-theory` →
`understanding-operator-co-await` → `understanding-the-promise-type` →
`understanding_symmetric_transfer` (**bài quan trọng nhất với bạn**: về việc tránh stack overflow
khi resume chuỗi, đúng cái xảy ra khi simulator chạy hàng triệu event). ⚠️ Series này **chưa hoàn
thành**: đừng mong nó cover cả một scheduler.

---

## PHẦN 7: LINUX MULTICAST: HAI THỨ SẼ CẮN BẠN

Cả hai đều thiếu tài liệu trong OSS:

- **`IP_MULTICAST_ALL`**: mặc định, một socket nhận traffic của **mọi group đã join trên host**,
  không phải chỉ group nó join. Phải set về 0, không thì bị rò rỉ cross-channel âm thầm. Đây là
  bug thật trong phần lớn multicast receiver mã nguồn mở.
- **`SO_REUSEADDR` vs `SO_REUSEPORT`**: `SO_REUSEADDR` là cái cho phép nhiều socket bind cùng
  group/port để fan-out. `SO_REUSEPORT` thì **load-balance**, tức là ngược lại điều một feed
  reader muốn. Nói đúng được sự khác biệt này trong văn bản là một điểm khác biệt thật.

Tài liệu: `man7.org/linux/man-pages/man7/ip.7.html` (đặc biệt `IP_ADD_SOURCE_MEMBERSHIP`, SSM,
feed sàn thật hay dùng), `docs.kernel.org/networking/snmp_counter.html` (thẩm quyền về
`UdpInErrors`/`RcvbufErrors`, tức `netstat -su` thật sự nghĩa là gì),
`man8/tc-netem.8.html` + `man8/ip-netns.8.html` + `man4/veth.4.html` (**đây là harness test**:
tiêm loss/latency/reorder/duplicate không cần hardware),
`blog.cloudflare.com/how-to-receive-a-million-packets/`, `rigtorp.se/sockets/`.

**Lập kế hoạch quanh điều này ngay:** AWS **không hỗ trợ multicast trên VPC thường** (cần Transit
Gateway multicast domain), và IGMP hành xử khác dưới veth/netns so với switch thật. Với ràng buộc
cloud-only, **toàn bộ test cục bộ phải là netns/veth hoặc loopback**, và IGMP snooping/querier:
nguyên nhân vận hành số 1 của "feed tự nhiên đứng": là thứ bạn sẽ phải suy luận chứ không
reproduce được. **Ghi rõ điều này trong README.** Giới hạn được khai báo là tài sản; giới hạn bị
người khác phát hiện là vết thương.

---

## PHẦN 8: CRAFT: LÀM SAO ARTIFACT ĐƯỢC CÔNG NHẬN

### Repo: điều thực sự tạo uy tín

Đã pull metadata và README đầy đủ của `rigtorp/hiccups` (136★, **1 CI job, 0 test dir**),
`max0x7ba/atomic_queue` (1.880★), `odygrd/quill` (2.981★). **Phát hiện chính: uy tín không đến từ
số badge hay kích thước repo. Nó đến từ việc công bố phương pháp và trung thực về giải pháp thay
thế.**

`hiccups`(artifact mà dossier nói là loại đi được trong kênh CppCon) làm những việc này:

- **Giải thích phương pháp bằng văn xuôi TRƯỚC khi đưa ra bất kỳ con số nào.**
- **Công bố cách dẫn ra threshold**: *"threshold được tính bằng 8 lần hiệu nhỏ nhất giữa hai
  timestamp liên tiếp trong 10000 lần chạy."*
- **Ghi công prior art**: `sysjitter` của David Riddoch.
- **Nêu tên một giải pháp TỐT HƠN chính nó và nói vì sao tốt hơn**: Linux osnoise tracer *"cũng đo
  system jitter, và còn chỉ ra nguồn gây jitter."* → **Chỉ người đọc tới thứ tốt hơn tool của
  mình là tín hiệu uy tín mạnh nhất có thể có.**
- **Báo p99 / p99.9 / max: không bao giờ báo mean.**
- **Không tính từ.** Mô tả là *"Measures the system induced jitter…"*: làm gì, không phải nhanh
  cỡ nào.

`quill` là mẫu mực về công bố percentile: có section **"System Configuration"** ĐỨNG TRƯỚC mọi con
số: OS, CPU + clock chính xác, version compiler, và **paste nguyên `/proc/cmdline`**
(`isolcpus=1-5 nohz_full=1-5 mitigations=off processor.max_cstate=1 intel_pstate=disable`). Code
benchmark ở một **repo công khai riêng** để ai cũng chạy lại được. Bảng cho 50/75/90/95/99/99.9:
**không có mean ở đâu cả**. Và **nước đi quyết định: quill công bố cả những bảng mà nó thua**,
XTR và NanoLog thắng nó ở p50–p99; quill chỉ thắng ở p99.9. Rồi phần "Verdict" của nó **khuyên
dùng đối thủ** cho use case khác.

**Ghi chú về AI:** tree của `quill` có một file `CLAUDE.md`, và nó là một trong những repo đáng tin
nhất trong lĩnh vực. **Dùng AI không phải là điều bị loại: output AI không được review mới bị
loại.** Dấu hiệu trong vụ bị CTO gọi tên là **bề rộng không được review**: nhiều chủ đề nói rất tự
tin, không có phép đo nào, không có failure mode nào.

**Checklist theo mức tín hiệu mang lại:** (1) phát biểu phương pháp cạnh mọi con số, máy, lệnh,
statistic, số lần chạy; (2) percentile, không bao giờ mean; (3) **một bảng hoặc kết quả bạn
không thắng**, có nêu tên đối thủ; (4) failure mode và caveat nói thẳng; (5) nêu tên giải pháp tốt
hơn nếu có; (6) ghi công prior art; (7) harness người lạ chạy được; (8) mô tả chính xác, **không
tính từ**; (9) CI thật sự chạy test + sanitizer; (10) có license; (11) test trong framework nhận
diện được; (12) badge: **tín hiệu thấp nhất**.

### Bài viết: template rút ra từ `rigtorp.se/ringbuffer/`

Bài đó **1.522 từ, 10 code block, 0 chart**. Bài học đầu tiên: bài mẫu thì **ngắn**.

Các nước đi của nó, theo thứ tự: (1) một câu scope; (2) headline là delta trước→sau so với
baseline có tên tuổi: *"tăng throughput từ 5,5M lên 112M items/s, thắng cả Boost và Folly"*;
(3) link implementation production ngay; (4) neo vào ứng dụng thật (NIC ring, io_uring CQ);
(5) xây từ bản naive, show full code; (6) **giải thích tham số benchmark VÀ nói rõ mình cố ý KHÔNG
đo cái gì(ngay tại chỗ quyết định, không phải ở footer**; (7) recipe reproduce chính xác) tên
file, dòng compile, CPU, cách đặt thread, lệnh chạy; (8) **paste nguyên counter `perf stat`**:
**con số được giải thích bằng cơ chế, không phải chỉ được khẳng định** (nước đi uy tín nhất bài);
(9) mời verify; (10) section "Further optimizations" = ranh giới scope trung thực.

**Nguyên tắc chuyển giao:** mọi con số đến kèm (a) lệnh sinh ra nó, (b) máy nó chạy, (c) cơ chế
giải thích nó, (d) phát biểu về chỗ nó không tổng quát hoá được.

**Bản chuyển thể cho bạn.** Dự án của bạn là dự án *đúng đắn*, nên thứ tương đương `perf stat` là
**counterexample đã rút gọn cộng invariant bị vi phạm**:

> **Claim** (kiểm chứng được, một câu) → **repo một lệnh kèm seed** → **fault schedule tối thiểu**
> (đã delta-debug từ trace thô) → **invariant bị vi phạm** → **cơ chế**, vì sao gap logic của
> handler đó fail → **giới hạn scope**: version nào, config nào, cái gì *không* test →
> **prior art và giải pháp thay thế**, nêu tên công bằng.

Và: khai báo ràng buộc cloud-VM **trước khi ai hỏi**, chỉ có software timestamp, không PMC, không
hardware timestamping.

### Kênh công bố: trạng thái CFP thật, tính đến 2026-07-29

| Kênh | Trạng thái |
|---|---|
| **CppCon lightning talk** | ⭐ **Cơ hội gần nhất tốt nhất.** Submit qua form ở `cppcon.org/lightning-talks-and-lightning-challenge/`. **5 phút.** Không có deadline công bố. Và quan trọng: *"các session lightning talk mở cho bất cứ ai, bất kể có vé hội nghị hay không – **kể cả nếu bạn muốn nói**!"* |
| CppCon 2026 full session | **ĐÃ ĐÓNG** (chạy 17/04 → 17/05/2026). Hội nghị 12–18/09/2026, Aurora CO, chỉ onsite. |
| **P99 CONF 2026** | 21–22/10/2026, online, **miễn phí**. CFP **đã đóng** (05/01 → 29/05/2026). Format: **15–20 phút pre-record**. Có track **"Measurement (tools, tracing, benchmarking, observability)"**(khớp trực tiếp. **CFP 2027 có thể mở ~01/2027) đặt nhắc lịch.** |
| **Show HN** | Mở luôn. ⚠️ **Bài viết của bạn KHÔNG thể là Show HN**: *"blog post... không chạy thử được nên không thể là Show HN."* **Tool** thì đủ điều kiện. |
| Meeting C++ / ACCU / Core C++ / C++ on Sea | Site live, ⚠️ không trích được deadline CFP hiện tại. |

**Deadline ngay:** đăng ký volunteer CppCon 2026 **đóng 01/08/2026, còn hai ngày.** Volunteer là
cách rẻ và hợp pháp để có mặt trong toà nhà cùng các sponsor đó tháng 9 này.

### Anti-pattern, kèm bằng chứng

| Anti-pattern | Bằng chứng |
|---|---|
| **"Ultra-low-latency" trong tiêu đề** | Bài HN bị CTO của hãng HFT gọi tên có tiêu đề **đúng y** *"Ultra-Low-Latency Trading System"* (story 46384415, 25/12/2025). Đối lập: `hiccups`, *"Measures the system induced jitter…"*, không tính từ. |
| **Repo trông như LLM** | Nguyên văn comment 46387677 của `raviolo`: *"CTO của một hãng HFT đây. Ý kiến của tôi: repo (và có lẽ cả comment của tác giả) là do LLM sinh ra... **Dù sao cũng tiết kiệm cho bạn một prompt 'generate low-latency trading system'.**"* → Câu cuối là điều tệ nhất có thể nói về một artifact portfolio: **nó rút gọn công sức thành một prompt.** |
| **Số không đo / bịa** | Thuốc giải là `/proc/cmdline` nguyên văn của quill và section Methodology của atomic_queue. **Nếu không paste được lệnh và máy, đừng công bố con số.** |
| **TPS từ vòng lặp không chạm data structure** | Phòng thủ là `perf stat` của rigtorp(chứng minh **cơ chế**) cộng công bố bảng mình thua. |
| **Mean thay vì percentile** | Cả `hiccups` và `quill` **không báo mean ở bất cứ đâu.** |
| **50 file `*_REPORT.md` ALL-CAPS** | Toàn bộ tree của `hiccups` có 7 entry. Cả ba repo uy tín: **0 file markdown báo cáo trạng thái.** |

---

## PHẦN 9: THỨ TỰ XÂY (đã điều chỉnh theo phát hiện mới)

1. **Ngày 1: đặt `coryan/jaybeams` làm CI fixture bắt buộc-phải-fail**, trước cả target thật.
   Nó validate chính harness của bạn, miễn phí.
2. **Tải file IEX HIST đầu tiên** (§1.1), viết reader pcap+pcapng, xử lý tag VLAN, parse IEX-TP,
   xác nhận 0 đứt sequence trên dữ liệu sạch. Đây là baseline "sạch" của bạn.
3. **Lấy OMI MoldUDP64 pcap** (§1.2a) để có framing MoldUDP64 thật của NASDAQ.
4. **Xây `chaos` trước `recovery`.** Nhỏ hơn, và cho phát hiện đầu tiên ngay lập tức: chạy nó lên
   feed handler của *người khác* trong danh sách Tier 1.
5. **Đánh giá `jamisonrobey/nasdaq-moldudp64-feed-sim` trước khi tự viết transmitter**: test gap
   *recovery* bất khả thi nếu không có gì trả lời re-request.
6. **`recovery`**: giờ đã có thứ để test nó bằng. Bắt đầu từ semantics của `nassau`.
7. **Delta-debug (ddmin) mọi failure** xuống counterexample tối thiểu trước khi công bố.
8. **`mock-exchange` phần OUCH order entry**: nặng nhất, để sau.

---

## Ghi chú về độ tin cậy

**Đã xác minh byte-level:** framing BinaryFILE của NASDAQ (24 message giải mã khớp struct ITCH
5.0); IEX HIST là pcap wire-format với VLAN + IEX-TP (20.145 packet, 0 đứt); chuyển pcap→pcapng ở
20170615/20170620; OMI MoldUDP64 pcap (399.999 chuyển tiếp liền mạch, 0 đứt); kích thước file B3
FeedA/FeedB.

**Đã xác minh bằng đọc source:** cả 9 target Tier 1 (đường dẫn file và số dòng ghi trong bảng);
`jaybeams` log-and-continue; `nixiz` là fork của `helix`.

**Chưa xác minh được: đã ghi rõ, không đoán:** quy tắc r/cpp (Reddit chặn JSON); hoạt động của
group mechanical-sympathy; `cppnow.org` (403 bot-block); deadline CFP của Meeting C++/ACCU/Core
C++/C++ on Sea; chất lượng codegen C++ của Kaitai (quan trọng: nó quyết định `.ksy` có sinh được
fault injector hay chỉ sinh được decoder); thời lượng spin của Glimpse (NASDAQ không công bố: số
"hàng chục giây" là ước lượng).

**Caveat của sweep:** GitHub cap mỗi query ở 1000 kết quả (`MoldUDP64` báo `total_count=1676`,
`SoupBinTCP` 2056: chỉ ~100 đầu mỗi loại được kiểm); `search/code` chỉ index default branch;
mọi tuyên bố latency trong mọi repo ("39.8ns limit add", "sub-25ns", "43-cycle deterministic")
đều **chưa được xác minh**.

**Còn đang chạy:** phần chính của luồng deterministic simulation engineering (thiết kế scheduler
C++20 coroutine, danh sách determinism leak phải bịt, loom vs shuttle). Phụ lục của nó đã về và
nằm ở Phần 10.

---

## PHẦN 10: DETERMINISTIC SIMULATION: CRAFT

### 10.1 ⚠️ Đừng lặp lại folklore về FoundationDB: reviewer sẽ bắt

Ba điều được truyền miệng khắp nơi và **đều sai hoặc yếu hơn nhiều so với danh tiếng**:

**a) Con số "một nghìn tỉ CPU-hours" KHÔNG nằm trong paper SIGMOD 2021.** Nó chỉ có trong
`testing.rst`, và câu đó **không đổi từ commit `3b86576b6`, ngày 2018-03-06**: bản copy
marketing thời tiền-Apple, chưa từng được refresh. Nó tự dán nhãn là estimate *"tương đương"*,
"weighted by the increased intensity of the failures in our scenarios". Và 10¹² CPU-hours ≈ **114
triệu CPU-năm**, tức là không thể đạt được theo nghĩa nguyên văn. → Nếu dùng, hãy trích là
*"estimate mà chính FoundationDB ghi trong tài liệu"*, **không bao giờ** gọi là số đo, và
**không bao giờ** dẫn về paper. (Con số 0,5 triệu disk-năm thì *thật* và đúng là từ paper, §6.2.)

**b) FoundationDB CHƯA BAO GIỜ được Jepsen kiểm tra.** Index `jepsen.io/analyses` không có FDB.
Bài "FoundationDB passed Jepsen" được trích khắp nơi là **tự làm**, do một nhân viên
FoundationDB viết năm 2014. Câu của Kingsbury tồn tại, nhưng là một **tweet ngày 25-11-2013**
(`@aphyr`, id `405017101804396546`, phục hồi qua Wayback): *"haven't tested foundation in part
because their testing appears to be waaaay more rigorous than mine."* **Mười ba từ, hedge hai lần**
("in part", "appears to be"), từ trước cả vụ Apple mua lại và trước khi mã nguồn được mở.

**c) Câu nên dùng thay thế**: Kingsbury, HN 2024 (`news.ycombinator.com/item?id=42120771`), là
phát biểu mạnh nhất của luận điểm bổ sung, kèm một sự nhượng bộ:

> *"Tôi đã làm vài dự án dùng simulation testing và pass hết, rồi vẫn phát hiện bug nghiêm trọng
> bằng Jepsen. **Khám phá không gian trạng thái và thiết kế oracle là những bài toán khó.**
> Jepsen tồn tại để test database bất kỳ của bên thứ ba, không cần họ hợp tác, thậm chí không cần
> access source. Test suite của FoundationDB được thiết kế để test FoundationDB."*

Đây là câu nên nằm trong write-up của bạn, vì nó nói đúng giới hạn của chính phương pháp bạn dùng.

### 10.2 Thiết kế shrinker: choice sequence có kiểu, không phải byte string

**Hypothesis không còn shrink một dãy byte.** Nó shrink một **typed choice sequence**:
`ChoiceNode{type ∈ {integer,float,boolean,bytes,string}, value, constraints, was_forced}`, với
shortlex định nghĩa trên `(len(nodes), tuple(choice_to_index(v, constraints)))`. Một
`vector<uint32_t>` chạy được nhưng **mất đi tính chất giá trị nhất: choice có kiểu và có ràng buộc
làm cho misalignment lúc replay trở nên PHÁT HIỆN ĐƯỢC. Một stream integer thô sẽ âm thầm diễn giải
sai.**

Citation gốc: **MacIver & Donaldson, "Test-Case Reduction via Test-Case Generation: Insights from
the Hypothesis Reducer", ECOOP 2020, DOI 10.4230/LIPIcs.ECOOP.2020.13.** §2.2 là lập luận
seed-vs-tape ở dạng hình thức: *"xem generator ngẫu nhiên như parser của choice sequence, với PRNG
là interface stream"*, và §3.2 là insight làm nó khả thi: *"dù ta không có grammar cho ngôn ngữ,
ta có parser: chính cái generator."* **Đọc §2.2 và §3.1–3.3 trước khi viết shrinker.**

Năm quy tắc thiết kế, mỗi cái đều xứng chỗ:

1. **Ghi span.** Instrument `draw` để push/pop `(label, start, end)`. Không có ranh giới span thì
   deletion là `O(n³)` candidate; có thì `O(n)`. RAII guard: `auto g = cs.span(EventKind::ApplyDelta);`
2. ⭐ **Đừng bao giờ draw một length rồi loop.** Hãy draw một bool "còn tiếp không" trước mỗi phần
   tử. Khi đó xoá một element = xoá một **vùng liền nhau**; với length prefix thì phải tìm một cặp
   `O(n²)` (giảm length + xoá vùng). **Chỉ riêng quy tắc này là khác biệt giữa một shrinker chạy
   được và một shrinker bị treo.** Bonus miễn phí: `[[1,2],[3,4]]` tự shrink thành `[1,2,3,4]`.
3. **`shrink_towards` theo từng draw, với zigzag index.** `choice_to_index` xếp integer theo
   `[a, a+1, a-1, a+2, a-2, ...]` quanh một target riêng cho từng draw, không theo thứ tự số học
   thô. Cho phép khai báo cục bộ "latency đơn giản nhất là 0", "instrument đơn giản nhất là index
   0". ~30 dòng, dễ chuyển sang chỗ khác.
4. **Khi misalign thì REALIGN, đừng abort.** Shuttle *panic* khi divergence; Hypothesis thay bằng
   **giá trị đơn giản nhất ở index 0** cho type+constraints đang được yêu cầu, tăng
   `misaligned_count`, rồi tiếp tục. Chạy quá prefix đã ghi là `OVERRUN` = "không interesting",
   không bao giờ là crash. Báo `misaligned_count` theo từng pass như một metric chất lượng pass.
5. **Reduction hai tầng, vì scale của bạn nằm ngoài vùng đã được kiểm chứng.** Hypothesis cap
   choice sequence ở `BUFFER_SIZE = 8 * 1024` và paper tự nêu đây là threat-to-validity chính:
   *"phần lớn test-case reducer gặp vấn đề ở scale lớn mà họ không thấy ở scale nhỏ."* Run 10M
   event của bạn nằm **rất xa** ngoài vùng đó. → **Stage 0:** bisect phần suffix xoá được (một
   failure ở 10M event thường phát sinh gần cuối). **Stage 1:** tỉa theo nhân quả, từ event fail,
   đi ngược happens-before và xoá mọi choice không thuộc ancestor nào; **không cần re-execute** nếu
   bạn đã ghi dependency edge. **Stage 2:** shortlex pass trên phần còn lại, với `max_stall=200`,
   `MAX_SHRINKS=500`, `max_failures=20` mỗi pass, và tự sắp lại thứ tự pass theo (độ dài xoá được,
   số shrink, số call).

Hai món thêm: **swarm flag "shrink mở"**, draw một mask trên các loại fault với baseline 0 nghĩa
là *bật hết*, để shrink đi về phía *nhiều* fault bật hơn và repro tối thiểu được phát biểu ở cấu
hình dễ dãi nhất. Và **corpus hai tầng** (primary = các ví dụ đã minimize, mỗi cái từng chứng minh
một bug khác nhau, sort shortlex; secondary = downsample mọi thứ interesting còn lại,
`desired_size = max(2, ceil(0.1 * max_examples))`): chỉ là một directory `.dst-corpus/` chứa tape
đã ghi, commit vào repo.

**Về ddmin, để so sánh:** đảm bảo 1-minimality, worst case `|c|² + 3|c|` test, best case
logarithmic. Tradeoff đo được trên Csmith: C-Reduce 120 byte / 3.968 lần gọi SUT, vs Hypothesis
812 byte / **762** lần gọi. Internal shrinking mua được **~5× ít execution hơn** và output luôn
hợp lệ, đổi lấy minima lớn hơn. **Với bạn đó là trade đúng, vì mỗi execution là một simulation 10M
event.**

### 10.3 Oracle: hai thứ tiết kiệm thời gian nhất

**a) Phân hoạch theo instrument, và lấy ý tưởng từ type signature của Porcupine.** Kiểm tra
linearizability toàn cục là bất khả thi, nhưng `Model` trong `anishathalye/porcupine` có
`Partition func(history []Operation) [][]Operation`: *"một history là linearizable khi và chỉ khi
mỗi phân hoạch là linearizable"* (P-compositionality, Horn & Kroening, arXiv:1504.00204). **Order
book của bạn là P-compositional theo symbol gần như theo định nghĩa.** Điều đó biến một phép kiểm
bất khả thi thành N phép kiểm độc lập khả thi, và được tuyên bố nhanh hơn Knossos 1.000–10.000×.
Trick bổ sung của Knossos: cache khoá theo **(tập op đã linearize, trạng thái model)**, vì hai
đường linearize cùng một *tập* vào cùng một trạng thái là thay thế được cho nhau.

**b) Năm checker của TigerBeetle, ở `src/testing/cluster/`:** `state_checker.zig`,
`storage_checker.zig` (byte-identical storage giữa các replica ở mỗi checkpoint: header của nó
**liệt kê mọi vùng bị loại trừ kèm lý do**, đó là mẫu cho một determinism oracle không bị flaky),
`journal_checker.zig`, `grid_checker.zig`, `manifest_checker.zig`, cộng `aof.zig::validate()`
(đọc lại log trên đĩa cuối run, so với checksum canonical cuối cùng). **Copy nguyên xi.**

**Bốn oracle có giá trị/dòng-code tốt nhất cho domain của bạn:**

1. ⭐ **Differential snapshot+incremental vs continuous-live.** Chạy cả hai pipeline trên cùng event
   stream trong cùng simulation; book phải giống nhau ở mọi điểm quiescence. Cái này cover đúng
   đường snapshot/recovery: nơi bug market-data thật sống, và là đường **ít được chạy nhất trong
   production**.
2. **Full-book recompute equality.** Replay ngây thơ toàn bộ ingest log, so byte-for-byte với book
   duy trì kiểu incremental. Một hàm, bắt được mọi bug về intrusive list và về việc duy trì
   aggregate.
3. **Hash chain trên output stream đã publish.** `parent = checksum(message publish trước đó cho
   instrument này)`: bắt reorder, duplicate, truncate và divergence trong một phép kiểm, và đồng
   thời làm canary phía consumer trong production.
4. **Conservation.** `sum(level_sizes) == sum(applied_deltas)`: bắt sign error, double-apply và
   lost-apply cùng lúc.

**Món nên lấy từ Jepsen:** `checker/set-full` duy trì timeline add/stable/lost cho từng element
**chuyên để phân biệt *mất* với *chưa hiện***: đúng bài toán dropped-vs-late của bạn.

**Vì sao đường recovery đặc biệt xứng đáng được chú ý**: từ trang error-injection của sled, dẫn
Yuan et al., OSDI'14: *"gần như tất cả (**92%**) các catastrophic system failure là kết quả của
việc xử lý sai các lỗi không nghiêm trọng đã được báo hiệu tường minh trong software"* và
*"ở **58%** các catastrophic failure, fault gốc có thể đã được phát hiện dễ dàng bằng cách test đơn
giản phần code xử lý lỗi."*

### 10.4 Coverage: lấy mẫu số từ chính cấu trúc bạn khai báo

Đây là bài học tổng quát hoá được từ Coyote: nó báo tuple `(machine, state, event)` dưới dạng
**covered ÷ defined**, lấy `defined` từ bảng handler mà actor khai báo (và ship `.coverage.ser` để
merge nhiều run).

Ma trận tự nhiên của bạn:
`instrument_state ∈ {uninitialized, snapshotting, live, gapped, recovering, stale, halted}`
× `event ∈ {snapshot, delta, trade, heartbeat, gap_detected, retransmit_arrived, session_reset,
subscriber_slow, subscriber_disconnect}` = **63 ô.**

**In ra những ô rỗng.** Mỗi ô rỗng là hoặc dead code, hoặc một transition chưa được test, và bạn
sẽ bất ngờ vì có bao nhiêu ô rỗng.

Ghi chú: Stateright độc lập phát minh lại sometimes-assertion thành `Property::sometimes` với
`discovery_is_failure() == false`; Resonate độc lập phát minh lại nó thành một *stopping rule*
(chạy đến khi cả 22 loại operation đều đã trả 2xx, rồi đến khi không có operation signature mới
trong 20 batch liên tiếp). **Ba dự án không liên quan hội tụ vào cùng một primitive là tín hiệu
mạnh rằng nó đúng.**

### 10.5 Canary và bảng mutation: xây ngày đầu tiên

**Canary.** TigerBeetle ship `src/scripts/cfo.zig` có một fuzzer target tên **`canary` được thiết
kế để LUÔN LUÔN FAIL**, với **120 lần canary fail được ghi** trong ledger công khai. Đó là một
meta-test chạy vĩnh viễn, ai cũng audit được: **nếu canary có lúc nào pass, thì harness đã hỏng.**
Một target, giá trị vô hạn. **Xây ngày đầu.**

**Bảng mutation**: artifact đo **độ nhạy** thay vì đo **khối lượng**, và khối lượng không có độ
nhạy chính là thứ đã để bốn fuzzer của TigerBeetle chạy qua hàng triệu seed mà không thấy bug
zig-zag join:

| Mutant | Phải vi phạm | Số seed để phát hiện | Wall |
|---|---|---|---|
| `off_by_one_seq` (accept `seq == last`) | tính đơn điệu của seq | 1 | 0,3 s |
| `drop_gap_request` (không bao giờ request retransmit) | liveness gap→recovery | 3 | 1,1 s |
| `stale_snapshot` (delta trước snapshot) | thứ tự dependency | 12 | 4 s |
| `torn_snapshot` (apply nửa vời khi restart) | differential snapshot/live | 40 | 15 s |

Chạy nightly, và coi **hồi quy về chi phí phát hiện là build failure**: nếu `stale_snapshot` từng
cần 12 seed mà giờ cần 400, thì một generator đã bị thu hẹp. Mỗi bug thật bạn từng sửa trở thành
một dòng mới. Đây đúng là điều TigerBeetle đã làm **một cách phản ứng** với #2681 sau khi bị Jepsen
làm cho mất mặt.

### 10.6 Tốc độ: đo của mình, đừng thừa hưởng "1000×"

| Nguồn | Speedup | Loại |
|---|---|---|
| RisingWave | **4–5×** (8 phút → 2 phút e2e) | **đã đo**, streaming CPU-bound |
| FoundationDB | ~10× (*"khoảng hệ số 10-1 giữa thời gian thật và mô phỏng"*) | tự báo |
| TigerBeetle | 700–1000× | tự báo |

Khoảng cách đó không phải marketing: nó đúng là số hạng CPU-utilization trong chính câu của FDB:
*"có thể chạy nhanh hơn real-time tuỳ ý nếu CPU utilization trong simulation thấp, vì simulator có
thể fast-forward clock tới event kế tiếp."* **Một feed bursty với nhiều khoảng lặng dài và nhiều
timer nằm ở đầu thuận lợi. Nhưng hãy đo và công bố hệ số của chính bạn.**

Throughput thật của TigerBeetle, tính độc lập từ ledger công khai (`devhubdb/fuzzing/data.json`,
479 record): **96.259.250 seed trên 31 commit trong một tháng, ~3 triệu seed/ngày** toàn fleet trên
23 target, với 37 seed `vopr` fail được giữ lại.

### 10.7 Uy tín, xếp hạng lại, và số liệu volume xếp CUỐI

1. ⭐ **Một bài viết công bố về bug mà DST của bạn ĐÃ BỎ SÓT, kèm cơ chế.** Bài
   `2025-06-06-fuzzer-blind-spots-meet-jepsen` của TigerBeetle giá trị hơn mọi con số performance
   cộng lại, vì nó là artifact duy nhất **có thể đã bị che đi mà họ không che**. Bốn fuzzer bỏ sót
   một đường merge-join zig-zag (*"sinh ra các object tình cờ nằm liền nhau trong mỗi index, nên
   phần 'zig-zag' của merge join chưa bao giờ được chạy"*); VOPR bỏ sót hai bug corruption vì
   *"nó corrupt cả sector, thay vì từng bit."*
2. **Validation độc lập của bên thứ ba.** TigerBeetle có report Jepsen thật
   (`jepsen.io/analyses/tigerbeetle-0.16.11`, 2025-06-06) và nó **ghi công cho DST của họ**:
   *"Chúng tôi cho rằng độ bền này phần lớn nhờ vào simulation, integration và property-based test
   rộng khắp của TigerBeetle."* FoundationDB, với tất cả danh tiếng, chỉ có một tweet 2013 và một
   bài blog tự làm.
3. Một artifact repro publish được cho từng failure (hex string hoặc `(seed, commit)`).
4. **Một seed ledger công khai, luôn bật**: khả năng audit thắng lời khẳng định; chính nó cho phép
   ledger ở §10.6 được kiểm chứng độc lập thay vì phải tin.
5. Bảng độ nhạy mutation.
6. Postmortem bug gắn với một seed cụ thể.
7. ⭐ **Một phát biểu bằng văn bản, tường minh, về RANH GIỚI của simulation.** FDB có §4
   Limitations; Dropbox viết *"Trinity không thể thật sự reboot máy giữa test, nên nó không thể
   validate rằng chúng tôi dùng fsync ở đúng mọi nơi"*; frostdb để chữ "(mostly)" ngay trong tiêu
   đề. **Một tuyên bố DST không kèm ranh giới thì đọc như marketing.**
8. Self-check tính tất định mỗi run, cộng canary. Dropbox chạy lại từng seed và assert cùng trạng
   thái cuối; Resonate chạy mỗi seed hai lần và diff log để **chính sự bất định làm fail build**
   (tự động file issue thật: `resonate-sdk-ts#414`). **Tính tất định suy giảm âm thầm.**
9. Lỗ hổng coverage là build failure.
10. **Số liệu volume: cuối cùng.**

### 10.8 Analogue gần nhất với dự án của bạn: Dropbox

Đáng đọc kỹ, vì nó là một **pipeline** chứ không phải hệ consensus: hai harness, **CanopyCheck**
trên riêng planner (có shrinking và cutoff 200 iteration cho vòng lặp vô hạn) và **Trinity** trên
toàn engine (mock FS/network/timer và **đảo thứ tự** các async request): *"hàng chục triệu lần
chạy test ngẫu nhiên"* mỗi đêm, và **CI tự tạo một task theo dõi cho từng seed fail, kèm commit
hash**. Dropbox cũng là **ngoại lệ duy nhất** cho nhận định "không ai trong giới DST làm shrinking"
CanopyCheck có shrinking kiểu QuickCheck.

### 10.9 Còn chưa xác minh được

Paper PPoPP 2005 về synchronization coverage (DOI 10.1145/1065944.1065972) không lấy được từ ACM,
CiteSeerX hay hai mirror: citation chắc chắn nhưng phần mô tả metric là diễn giải lại, **hãy kiểm
trước khi trích.** Không dự án nào tài liệu hoá một chương trình mutation testing hệ thống cho
harness DST. Không có số liệu speed/scale công khai của madsim hay Antithesis (Antithesis từ chối
công bố có chủ ý). `apple/foundationdb/design/testing.md` **không tồn tại ở bất kỳ ref nào.** Và
nếu trích Gibbons & Korach cho tính NP-đầy đủ của linearizability, hãy dẫn trực tiếp kết quả VL
của họ, đừng dẫn câu related-work của Elle: câu đó thực ra gán *sequential consistency* cho họ.

---

## PHẦN 11: KIẾN TRÚC DST TRONG C++20

### 11.0 ⚠️ Đính chính quan trọng: FoundationDB ĐÃ chuyển Flow sang C++20 coroutine

Xác minh trực tiếp: `apple/foundationdb` trên `main` đã có `flow/include/flow/Coroutines.h` và
`flow/include/flow/CoroutinesImpl.h` (**1.497 dòng**); số file `.actor.cpp/.h` giảm từ **500 → 21**
giữa `release-7.3` và `main`; `Net2.actor.cpp` thành `flow/Net2.cpp` ngày **2026-04-19**. Migration
bắt đầu **2023-11-14**, commit `e07b3e35`, "Added C++ Coroutine support to Flow".
`Future<T>`/`Promise<T>`/`Sim2`/`BUGGIFY` bên dưới không đổi.

**Điều này sửa lại claim trong `RESEARCH-DOSSIER.md` §2.** Cách phát biểu đúng và mạnh hơn:
*implementation kinh điển đã bước qua cánh cửa đó(từ bên trong) rồi hàn nó lại sau lưng họ.*
Coroutine của Flow không tách rời được khỏi `Arena`/`Standalone`/`Reference`/`g_network`/
`allocateFast` và kỷ luật toàn codebase. **Chưa ai trích nó ra thành một library dùng lại được**, và
các phép tìm khoảng trống vẫn trả về không: `simulation testing coroutine deterministic` → **0**;
`virtual clock deterministic scheduler C++` → **0**; `deterministic executor coroutine language:C++`
→ **0**.

**Hệ quả thực tế: `flow/include/flow/CoroutinesImpl.h` là file giá trị nhất cần đọc trước khi viết
bất kỳ dòng code nào**: một DST engine production chạy trên C++20 coroutine native.

**Hai lựa chọn thiết kế trong đó chính là câu trả lời cho hai bài toán khó nhất của bạn:**

**a) Whitelist `await_transform` liệt kê đầy đủ, KHÔNG có generic fallback.** Cả 21 khai báo trong
`CoroutinesImpl.h` đều là overload cụ thể (`Future<U>`, `FutureStream<U>`, `AsyncResult<U>`,
`ThreadFutureStream<U>`, `coro::FutureIgnore<U>`, `coro::FutureErrorOr<U,V>`). **Không có**
`template<class A> await_transform(A&&)`.

Vì khi đã khai báo `await_transform`, *mọi* `co_await` trong thân hàm đều đi qua nó → `co_await
someAsioAwaitable` trở thành **lỗi compile**. Đây là cách mua lại tính đúng đắn **ở compile time**
mà actor compiler chưa bao giờ cho họ: chuỗi lỗi của actor compiler chỉ cấm những hình dạng
control-flow nó không lower được, nó **không cưỡng chế determinism chút nào**.

**b) Cấp phát frame nằm trong tay bạn.** Cả bốn promise type đều khai báo
`static void* operator new(size_t s) { return allocateFast(int(s)); }`. Cộng thêm một overload
placement-new `FrameSizeRecorder` để *quan sát* kích thước frame do compiler chọn, và
`ActorType coroActor; // Embedded in coroutine frame: single allocation`.

### 11.1 Khuyến nghị: tự viết `task<T>` + scheduler tối giản, ~800–1500 LOC, không phụ thuộc gì ngoài compiler C++20

Yêu cầu quyết định điều này: **bạn phải sở hữu ready queue, và mọi quyết định scheduling phải là một
hàm của seed và phải hiện trong trace.** Kéo theo đó là **mọi library đều bị loại.**

| Repo | ★ | Push cuối | Verdict |
|---|---|---|---|
| `lewissbaker/cppcoro` | 3.870 | 2024-01-09 | Ngủ đông, 113 issue mở. Đọc để học. |
| `andreasbuhr/cppcoro` | 449 | 2026-06-12 | Fork được bảo trì |
| `facebook/folly` | 30.480 | 2026-07-29 | Sống; nhưng nặng dependency |
| `David-Haim/concurrencpp` | 2.757 | 2025-05-01 | Sống |
| `alibaba/async_simple` | 2.192 | 2026-07-08 | **Hình dạng gần nhất.** Đọc để học. |
| `jbaldwin/libcoro` | 961 | 2026-05-02 | Sống |
| `Naios/continuable` | 851 | 2023-09-12 | Ngủ đông |
| `NVIDIA/stdexec` | 2.396 | 2026-07-29 | Sống |
| `netcan/asyncpp` |(|) | **404, không tồn tại** |

Vì sao từng cái fail: `io_service` của cppcoro dùng OS timer thật; `scheduler` của libcoro là thread
pool + `io_notifier`; thread pool của concurrencpp thuộc một `runtime`; asio có `io_context`. Seam
`Executor` của folly **đúng hình dạng** nhưng `folly-deps.cmake` bắt buộc Boost ≥1.69 + fmt +
libevent + OpenSSL + FastFloat. **`async_simple` gần nhất**(`virtual bool schedule(Func)`) nhưng
`Func = std::function<void()>` **type-erase mất `coroutine_handle`**, để lại một queue closure mờ
đục không có identity coroutine nào để sắp thứ tự hay trace. Đúng hình dạng, sai kiểu.

### 11.2 ⚠️ P2300 / `std::execution` nằm trong C++26, và nó LÀM HẠI

`std::execution::run_loop` bị loại bởi ba câu trong chính spec của nó:

1. `pop-front` / `push-back` là **private và exposition-only** → bạn **không thể** inspect, sắp lại,
   seed, hay single-step cái queue. Và **không có `run_one()`**.
2. Queue là FIFO thread-safe hardcoded → nó khám phá **đúng một** schedule.
3. Grep toàn bộ **9.470 dòng `exec.tex`** cho `schedule_after` / `schedule_at` / `now()` /
   `timed_scheduler` → **0 hit**. **C++26 `std::execution` KHÔNG có khái niệm timed scheduler nào cả**
  , mà virtual clock là toàn bộ lý do tồn tại của framework bạn.

Sender tồn tại để *trừu tượng hoá* scheduler đi, để algorithm generic không thể phụ thuộc vào nó.
Đó chính xác là tính chất bạn **buộc phải vi phạm**.

Trên toolchain của bạn (Apple clang 21, `_LIBCPP_VERSION = 210106`): **không có** `__cpp_lib_senders`,
thậm chí **không có** `__cpp_lib_generator` (C++23) ở bất kỳ `-std` nào. `__cpp_impl_coroutine =
201902`: core language có. **Xây trên C++20 thuần.**

### 11.3 Hình dạng executor

```cpp
void enqueue(std::coroutine_handle<> h) { ready_.push_back(h); }   // CHỈ enqueue. TUYỆT ĐỐI không resume.

void run() {
  for (;;) {
    while (!ready_.empty()) {
      auto h = pick(ready_);      // NƠI DUY NHẤT một quyết định scheduling xảy ra  <- seed vào ở đây
      ready_.pop_front();
      log(step_++, id_of(h));     // NƠI DUY NHẤT trace được ghi
      h.resume();                 // đúng một resume() sống ở bất kỳ thời điểm nào
    }
    if (timers_.empty()) break;
    now_ = timers_.begin()->first;  // virtual clock NHẢY. Không bao giờ sleep.
    drain_due_timers_into(ready_);
  }
}
```

Một chỗ gọi `resume()` duy nhất cho độ sâu stack giống nhau ở mọi lần resume, và một total order
trong trace. `pick()` là nơi seed vào: FIFO cho baseline, PCT cho exploration.

### 11.4 Symmetric transfer: đã đo, và đây là cái bẫy nguy hiểm nhất

Cùng một chương trình, hai dạng `await_suspend`, chạy trên máy M2 Max arm64:

```
naive (void await_suspend + resume inline):  100k → OK;  400k / 700k / 1M → exit 139 (SIGSEGV)
symmetric (return coroutine_handle<>):       400k / 700k / 1M → OK
```

**Đây đúng là bài toán của bạn, không phải corner case:** một simulated network với virtual clock sẽ
**hoàn thành `co_await recv()` một cách đồng bộ mỗi khi byte đã có trong buffer**: trong một
simulation zero-latency thì đó là phần lớn thời gian. **Một DST harness chính xác là loại workload
đâm vào vách trampoline.**

→ Return `coroutine_handle<>` từ `await_suspend` của **cả** task awaiter **và** final awaiter, và
return `std::noop_coroutine()` (**không bao giờ** trả handle default-construct) khi không có
continuation.

⚠️ **Việc bản naive sống sót ở 100k là điều làm nó nguy hiểm: nó pass unit test của bạn.**

### 11.5 Arena trả cổ tức hai lần

```
arena bump allocator:  2,9 ns/coroutine   (349M/s)
global new/delete:    17,8 ns/coroutine    (56M/s)     -> 6,1x
```

Và nó làm heap trở nên **checkpoint được**: snapshot arena rồi diff hai run để khu trú một
divergence, điều bất khả thi với `malloc`. Thêm `get_return_object_on_allocation_failure()` để
arena cạn thành một **fault OOM tất định, inject được**, thay vì một `bad_alloc` unwind xuyên qua
executor.

### 11.6 ⭐ DANH SÁCH DETERMINISM LEAK PHẢI BỊT

**Leak pointer-hash, và phần ai cũng làm sai.** Ba run cùng một binary:

```
str-iter: NFLX NVDA META TSLA AMZN GOOG MSFT AAPL   (giống nhau cả 3 run)
ptr-iter: 7 6 1 4 3 2 5 0
ptr-iter: 7 5 3 4 2 1 6 0
ptr-iter: 7 6 5 4 2 3 1 0
```

`unordered_map<string,int>` iteration là **tất định**; `unordered_set<Node*>` **không**. Xác minh
trong source: libstdc++ `bits/functional_hash.h:110-114` là
`hash<_Tp*>{ return reinterpret_cast<size_t>(__p); }`: **hash CHÍNH LÀ con trỏ**. libc++
`__functional/hash.h:344` hash các byte của con trỏ. Cả hai **không salt** string hashing
(libstdc++ seed cố định `0xc70f6907`), nên `std::hash<std::string>` ổn định, khác Python/Rust.

**Và đây là phần quan trọng: một arena tất định sửa được ordered container nhưng KHÔNG sửa được
hashed container** (4 run):

```
run 1: arena base=0x104904000  hash-by-POINTER: 7 6 5 1 4 3 2 0   hash-by-ID: 7 6 5 4 3 2 1 0
run 2: arena base=0x100de4000  hash-by-POINTER: 7 6 5 4 0 2 1 3   hash-by-ID: 7 6 5 4 3 2 1 0
run 3: arena base=0x102474000  hash-by-POINTER: 7 6 5 0 3 2 4 1   hash-by-ID: 7 6 5 4 3 2 1 0
run 4: arena base=0x102d84000  hash-by-POINTER: 7 5 4 3 2 6 1 0   hash-by-ID: 7 6 5 4 3 2 1 0
```

`std::set<Node*>` trên bộ nhớ arena thì *ổn định* (thứ tự tương đối sống sót qua việc base dịch),
nhưng `unordered_set` tính `bucket = ptr % 11` và base bị ASLR dịch làm đổi modulus.

> ⭐ **Quy tắc KHÔNG phải "dùng arena". Quy tắc là: đừng bao giờ để một giá trị con trỏ đi tới một
> hàm hash hoặc một comparator. Cho mọi object một ID đơn điệu từ bộ đếm có seed, và sắp thứ tự
> theo ID đó.**

FDB tới cùng kết luận; `contrib/debug_determinism/README.md` liệt nó là failure mode #2:
*"Phụ thuộc vào thứ tự tương đối của bộ nhớ đã cấp phát. VD: dùng con trỏ heap làm key trong
`std::map`."*

Tệ hơn: `std::set<Node*>` trên con trỏ **heap** thì **không nhất quán**, run 3 tình cờ ra đúng thứ
tự trong khi run 1–2 thì không. **Nondeterminism không nhất quán là loại khó debug nhất.**

| # | Leak | Cách bịt |
|---|---|---|
| 1 | `system_clock`, `steady_clock`, `high_resolution_clock`, `clock_gettime`, `gettimeofday`, `time()` | Inject một `Clock` concept / template param. Ban header trong build DST. |
| 2 | **`rdtsc`**: là một *instruction*, linker và `LD_PRELOAD` không chặn được | Chỉ shim ở compile time; **grep disassembly để chứng minh nó không có** |
| 3 | `random_device`, `rand()`, `getrandom`, `getentropy`, `arc4random`, `mt19937` seed mặc định | Mọi randomness qua đúng một `ChoiceSource` |
| 4 | **`std::uniform_int_distribution` và họ hàng**, `[rand.dist.general]` **không quy định thuật toán**, nên seed không replay được giữa libstdc++ và libc++ | Tự viết `draw(bound)` chỉ dùng integer. TigerBeetle đi xa hơn: `stdx.PRNG` **không có floating point nào**, dùng `Ratio{num,den}` và `chance()` integer |
| 5 | Hash dẫn từ con trỏ (`unordered_*<T*>`) | Không bao giờ hash con trỏ. ID đơn điệu. |
| 6 | **Thứ tự** dẫn từ con trỏ (`std::set<T*>`, `std::map<T*,V>`, `sort` trên con trỏ, con trỏ làm tie-break) | Total order trên ID; arena là phòng thủ lớp hai |
| 7 | ASLR / địa chỉ tuyệt đối lọt vào log hoặc phép so sánh | Log **offset** của arena, không bao giờ địa chỉ tuyệt đối. Chính test determinism của Shadow phải làm `sed 's/0x[0-9a-f]*/HEX/g'` trước khi diff: đó là dấu hiệu. |
| 8 | `std::thread`, `std::async`, thread ID, `hardware_concurrency`, `sleep_for` | Một OS thread duy nhất. Ban ở build time **và** thêm tripwire runtime |
| 9 | Thứ tự syscall: `epoll`/`kqueue` return order, short read, `mmap`, DNS, TCP coalescing | Trừu tượng ở **seam message**, không phải seam byte |
| 10 | Thứ tự `readdir`: đã xác nhận `directory_iterator` trả `alpha bravo charlie zeta mike yankee`, tức thứ tự filesystem, không sort | Sort tường minh, luôn luôn |
| 11 | **FP contraction**: xác minh ở mức instruction: `-ffp-contract=off` → `fmul d0,d0,d1`; `on` (**mặc định của Clang**) và `fast` (**mặc định C++ của GCC**) → `fmadd d0,d0,d1,d2` | `-ffp-contract=off`, không `-ffast-math`/`-Ofast`, ghi vào build fingerprint. **Cực quan trọng cho pipeline giá/VWAP.** |
| 12 | libm version drift (`sin`/`exp` không bit-identical qua các version), kích thước `long double`, denormal/FTZ, thứ tự `accumulate` | Tránh transcendental trong core tất định; **fixed-point cho giá** |
| 13 | **Bộ nhớ chưa khởi tạo.** FDB: *"99/100 lần, nguồn của nondeterminism là dùng bộ nhớ chưa khởi tạo"* | MSan/valgrind **bên trong** vòng lặp DST |
| 14 | Padding của struct trong struct được hash/serialize/`memcmp` | Serialize tường minh; không bao giờ `memcmp` một struct |
| 15 | Permutation không xác định của `std::sort` với phần tử bằng nhau | `stable_sort`, hoặc một total order không cho phép ties |
| 16 | Thứ tự `type_index`: ổn định trong một binary static, không an toàn qua shared library | Không sắp theo nó; static-link binary DST |
| 17 | Env var, thứ tự static-init giữa các TU, `__FILE__`/`__COUNTER__` trong output được so sánh | Init sequence tường minh; canonicalize log |
| 18 | Bất kỳ logic app nào đọc wall time: timeout, rate limiter, TTL cache | Tất cả qua virtual clock |
| 19 | **Version compiler/stdlib: reproducibility chỉ đúng cho từng binary.** Đã xác minh: cùng một chuỗi coroutine cấp phát 16 frame, tổng **704 byte ở `-O0`, 608 byte ở `-O2`** | Ghi build fingerprint (compiler, version, flag, stdlib) cùng mọi seed. **Một seed không kèm fingerprint không phải một repro.** |
| 20 | HALO elision âm thầm bỏ qua `operator new` của bạn | Đã xác minh cả hai chiều. Đừng dựa vào nó, và **tuyệt đối không dùng chung một PRNG stream giữa allocator và fault injector**: một allocation bị elide sẽ dịch cả cái tape. |

**Container an toàn:** `vector`, `deque`, flat map, intrusive list, B-tree khoá trên total order của
ID. **Bẫy:** bất kỳ `unordered_*` khoá trên con trỏ; bất kỳ ordered container khoá trên con trỏ; bất
kỳ thứ gì mà bạn chưa thiết lập thứ tự iteration một cách tường minh.

**Hai detector nên ship ngày đầu, cả hai từ FDB:**

- **Unseed fingerprint:** rút một integer từ PRNG lúc process exit, log nó, chạy lại seed, so sánh,
  một hash một-integer của "randomness đã bị tiêu thụ bao nhiêu lần và theo thứ tự nào", sample qua
  `unseed_check_ratio` với opt-out tường minh cho test vốn dĩ nondeterministic.
- **`contrib/debug_determinism/`:** build với `-fsanitize-coverage=trace-pc-guard`, ghi mọi edge id
  ra file, chạy lại, **halt ở edge đầu tiên khác nhau** để attach debugger đúng chỗ đó (~50 dòng).

### 11.7 loom vs shuttle: xây RANDOMIZED/PCT làm engine chính

**Đừng xây DPOR. Đừng xây simulator memory model C11.** Thêm bounded exhaustive kiểu exhaustigen cho
unit nhỏ, vì nó gần như miễn phí.

**Năm lý do:**

1. **Giới hạn của loom là hằng số layout dữ liệu, không phải knob.** `MAX_THREADS = 5` **tính cả
   main** (`VersionVec` là `[u16; 5]`); `MAX_ATOMIC_HISTORY = 7`, nên một bug cần store thứ 8 gần
   nhất là **vô hình**; mặc định 1.000 branch với panic khi tràn. Pipeline của bạn là feed handler →
   decoder → book builder → fan-out: **nhiều actor, run dài**.
2. **"Exhaustive" ≠ sound, và loom tự ghi điều đó.** SeqCst được model thành AcqRel → **false
   positive**; load buffering không được explore → **false negative**. Bạn trả full giá DPOR cộng
   giá memory model **mà vẫn không có chứng minh**.
3. **Bug bạn quan tâm là bug NÔNG.** Trong phần đánh giá của paper PCT, **mọi** bug đều có độ sâu
   d ∈ {1,2}, kể cả hai bug browser chưa từng biết (Mozilla, IE) tìm được ở d=1 trong chương trình
   có `n=25, k=1,4M` và `n=12, k=38,4M`. So với CHESS: PCT tìm cùng hai bug ở run 6 và run 35, chỗ
   CHESS cần ~200 và ~1000. **Stress testing được 0.**
4. **Chính bản port C++ của Microsoft đã chọn như vậy.** `cpp-systematic-testing` ship
   `StrategyType { Random, Prioritization, Replay }`: **không DFS, không memory model**.
5. **Stackless coroutine lập luận theo cùng hướng.** loom cần branch point ở *mọi atomic*, sâu trong
   call stack: đó là lý do cả loom và shuttle dùng coroutine **stackful**. C++20 coroutine là
   **stackless**: bạn chỉ suspend được ở một `co_await` trong frame của mình. **PCT chỉ cần quyết
   định ở điểm suspension, mà đó chính là nơi `co_await` đã ở.** Ngân sách cho hệ quả: `co_await`
   lây lan, nên mọi primitive blocking phải thành awaitable.

**PCT (Burckhardt, Kothari, Musuvathi, Nagarakatte, ASPLOS 2010).** Gán cho mỗi task một **priority
ngẫu nhiên lúc tạo**; luôn chạy task runnable có priority cao nhất; chèn `d − 1` **điểm đổi priority
chọn ngẫu nhiên**, và ở điểm thứ i thì hạ priority của task đang chạy xuống `i`. Bảo đảm, nguyên văn:
*"Cho một chương trình tạo tối đa n thread và thực thi tối đa k instruction, PCT tìm được bug độ sâu
d với xác suất ít nhất **1/nk^(d−1)**."* Với d=1, d=2 là `1/n` và `1/nk`. Tỉ lệ đo được thực tế cao
hơn bound tới **4 bậc độ lớn** (Dryad: 0,164 đo được vs 2×10⁻⁵ bảo đảm).
PDF: `microsoft.com/en-us/research/wp-content/uploads/2016/02/asplos277-pct.pdf`

**Chi tiết implementation nên copy từ shuttle** (`shuttle-schedulers/src/pct.rs`): **học `k` động**
(iteration 0 đo số step, chỉnh lên khi thấy run dài hơn: cái này diệt vấn đề thực tế lớn nhất của
paper); **chỉ đếm một step khi `runnable.size() > 1`** (phase optimization §4.1; Table 2 của họ cho
thấy `k` giảm 1,4M→0,13M và 38,4M→3M, tức cải thiện 10× trực tiếp ở d=2); chèn điểm **"Final Wait"**
của paper trước khi task main exit; coi `is_yielding` là một forced priority change. Thêm **fair
suffix của Coyote** (prefix PCT không công bằng rồi tail uniform-random, `MaxFairSteps = 10 ×
MaxUnfairSteps`) để spin loop và liveness assertion không tạo false livelock. Rồi chạy một
**portfolio** nhiều strategy × nhiều seed: câu trả lời trung thực của Coyote cho "strategy nào?" là
"nhiều cái".

**Primitive làm tan biến câu hỏi.** Đưa mọi quyết định nondeterministic qua một interface hẹp duy
nhất: shuttle chứng minh ba method là đủ (`new_execution()`, `next_task(runnable, current,
is_yielding)`, `next_u64()`): rồi thay implementation:

| Mode | Implementation | Cho bạn |
|---|---|---|
| Generate | xoshiro256++ có seed, **ghi lại** từng draw | DST randomized ở scale |
| Replay | phát lại chuỗi đã ghi | reproduction, CI regression |
| Shrink | phát lại chuỗi đã **mutate** | minimal counterexample |
| Exhaustive | odometer trên các cặp `(value, bound)` | bounded exhaustive (loom mode), **~90 LOC** |

TigerBeetle ship cái exhaustive: `src/testing/exhaustigen.zig` trình ra *cùng API với PRNG của họ*
nhưng liệt kê mọi choice sequence: giữ các cặp `(value, bound)`; để tiến, tăng value phải nhất còn
dưới bound của nó và zero phần còn lại (**odometer cơ số hỗn hợp trên một không gian được phát hiện
động**). Test của nó assert đúng n! permutation của "abcd". Cap độ sâu 32. Lý do trong bài "generate
all the things" của matklad: các sequence hiện ra **đã sắp theo độ phức tạp**, nên *"ví dụ fail đầu
tiên thực ra được bảo đảm là counterexample nhỏ nhất"*: **shrinking miễn phí ở scale nhỏ**.

### 11.8 FDB và TigerBeetle virtualize thế nào, và seed đi qua đâu

**FoundationDB.** `g_network` là một **global thay thế được**: `Net2` trong production, `Sim2` trong
simulation. Mọi thời gian đến từ `g_network->now()`; mọi I/O qua cùng interface đó. **Không có gì
trong application code gọi tên một clock thật.** Một lớp indirection duy nhất đó là toàn bộ kiến
trúc, và là lý do việc chuyển sang coroutine không làm xáo trộn nó: `Net2.cpp` đổi, `Sim2` không
cần đổi.

Randomness **tách theo tên để việc dùng sai trở nên thấy được**: `deterministicRandom()` vs
`nondeterministicRandom()`: hai hàm, và cái thứ hai là một code smell bạn grep được.

Storage: `fdbrpc/AsyncFileNonDurable.actor.h` model việc mất write chưa sync, write chưa `fsync` có
thể bị mất, apply một phần, hoặc bị đảo thứ tự khi crash mô phỏng.

**Network fault: "swizzle-clogging"**, pattern vô địch của họ, nguyên văn từ `testing.rst`:
*"chọn một tập node ngẫu nhiên... 'clog' (dừng) từng kết nối mạng của chúng, lần lượt, trong vài
giây... rồi unclog theo thứ tự ngẫu nhiên."* **Nên copy trực tiếp**: nó tạo ra partition bất đối
xứng, biến thiên theo thời gian, mà xác suất drop đều không bao giờ chạm tới.

⭐ **BUGGIFY: hai tầng, và đây là thiết kế then chốt.** Xác minh trong `flow/include/flow/Buggify.h`:
`P_BUGGIFIED_SECTION_ACTIVATED = 0.25` được quyết **một lần cho mỗi (file,line) mỗi run** và memoize
trong `*_SBVars`; `P_BUGGIFIED_SECTION_FIRES = 0.25` được đánh giá **mỗi lần** site đó chạy.

Đó là **swarm testing ở mức granularity dòng code**: mỗi run bật một tập con ~25% ngẫu nhiên các
site, giữ cho từng run vẫn sống sót được, trong khi cả tập hợp phủ được cross-product. **532 site
trên 66 file ở `main`** (778 ở `release-7.3`, trước migration). Giờ là một *function* dùng
`std::source_location`, không phải macro. `P_EXPENSIVE_VALIDATION = 0.05` đi cùng switch đó nên các
phép kiểm invariant O(n²) bật trong 5% run. Quyết định activation được trace, nên tập activation
chính xác của một run fail là phục hồi được. Alex Miller (`transactional.blog/simulation/buggify`)
nói rõ ý định: ***"làm việc xấu, nhưng đừng quá nhiều."***

**Swarm cả configuration, không chỉ fault.** Từ `testing.rst`: *"Randomize cả tuning parameter cũng
đảm bảo rằng các giá trị tuning performance cụ thể không tình cờ trở thành cần thiết cho tính đúng
đắn."*

Coverage: `CODE_PROBE`(**685 site ở `main`**) với static registration cho mỗi call site, nên một
probe **chưa bao giờ chạm tới vẫn xuất hiện** trong output. Aggregate toàn fleet bởi
`contrib/TestHarness2` **vào chính FoundationDB** qua counter atomic `tr.add()` conflict-free khoá
trên `(file, line, comment, rare)`: **khoá theo comment, nên refactor không reset lịch sử**.

**TigerBeetle.** Thời gian là tick-based, và **clock là một component hạng nhất có thể bị fault**:
skew và drift theo từng replica được inject để chính code clock-sync của họ (`src/clock.zig`) bị
test chứ không được giả định. I/O dưới mức tick được drain bằng một **vòng quiescence**:
`while (advanced) { network.step(); storage.step(); }`.

⭐ **Seed là một git hash.** `if (bytes.len == 40)` parse thành u160 rồi truncate, nên **mỗi commit
có một run reproducible miễn phí**, và mọi failure là một cặp `(seed, commit)`. `src/scripts/cfo.zig`
giữ `commit_count_max = 32`, `seed_count_max = 4`, `budget = 60 phút`, `timeout = 30 phút`, và **ưu
tiên seed fail nhanh hơn** (comment: `as coarse seed minimization`).

**Một PRNG duy nhất, chỉ integer.** `stdx.PRNG` trình ra `Ratio{num, den}` và `chance()`: **không
floating point ở đâu trong API random**, loại bỏ leak #4 và #11 ngay từ thiết kế.

**Swarm ở mức enum.** `fuzz.random_enum_weights` **tắt hoàn toàn một tập con ngẫu nhiên các variant
của enum** cho một run.

⭐ **Liveness vs safety: ý tưởng chuyển giao được nhất của họ.** Exit code phân biệt (`crash=127`,
`liveness=128`, `correctness=129`). Stall detector **reset khi có tiến triển**:

```zig
if (simulator.requests_replied > requests_replied_old) tick = 0;  // tick TÍNH TỪ REPLY CUỐI
```

Rồi hai phase: chạy có fault; sau đó chọn một **core** (tập liên thông mạnh chứa một quorum),
**chữa lành nó hoàn toàn** (latency 1 ms, xác suất fault bằng 0), và yêu cầu hội tụ trong
`ticks_max_convergence = 10_000_000`. `pending()` trả về `?[]const u8`: **một chuỗi LÝ DO**, không
phải bool. Then chốt: `cluster_recoverable()` phải *chứng minh* rằng một stall được giải thích bởi
fault đã inject **trước khi** báo bug: ~170 dòng dựng lại góc nhìn của cluster và **panic với
`"block found in core"`** nếu một replica thực sự đang giữ block mà replica khác đang chờ.

**Đòn tăng tốc.** Config `test_min` thu nhỏ không gian trạng thái (`journal_slot_count = 64`,
`block_size = 4096`, `lsm_compaction_ops = 4`) để **một lifecycle đầy đủ**: checkpoint, WAL wrap,
compaction, state sync: đạt tới được trong vài giây.

### 11.9 Mô hình performance, dẫn ra bằng thực nghiệm

Virtual time **nhảy tới event kế tiếp**, nên wall time là O(#event), **độc lập với thời lượng mô
phỏng**. Đo được 15–29M coroutine resume/giây đơn luồng (10M event trong 0,39–0,65 s), rồi chỉ thay
đổi khoảng cách virtual trung bình giữa các event:

```
GAP=10 tick      → 0,4x thời gian thật
GAP=1.000 tick   →   46x
GAP=100.000 tick → 4418x
```

→ **`speedup ≈ mean_virtual_gap_per_event / wall_time_per_event`**, với wall_time_per_event ≈
**34 ns**. **Để tuyên bố 1000× bạn cần ≥ ~34 µs virtual time cho mỗi global event.** Điều này giải
thích khoảng cách đã công bố: RisingWave 4–5× (đo, CPU-bound), FDB ~10× (tự báo, DB bận, event dày),
TigerBeetle 700–1000× (tự báo, nhiều đoạn idle dài).

### 11.10 Shadow: verdict, KHÔNG phải substrate của bạn

`shadow/shadow` sống (1.712★, push 2026-07-28). Đếm bảng dispatch trong
`src/main/host/syscall/handler/mod.rs`: **đúng 150 nhánh `SyscallNum::NR_* =>`**, gồm `clock_gettime`,
`getrandom`, `nanosleep`, `epoll_*`, `futex`.

**Nhưng đọc `docs/testing_determinism.md`, đó là phát biểu trung thực:** *"Nếu bạn chạy Shadow hai
lần với cùng seed... thì nó **nên** cho kết quả tất định (**nếu không thì đó là bug**)."* Tính tất
định ở đây là một **nguyện vọng kèm bug tracker**, không phải một bảo đảm. Và `docs/limitations.md`
ghi rõ `--native-preemption-enabled` gây *"mất tính tất định của simulation."*

**Sáu lý do loại:** (1) chỉ Linux, và `LD_PRELOAD` nghĩa là **binary static link không chạy được
gì cả**, mà bạn dev trên macOS/arm64; (2) **không có quyền kiểm soát scheduling trong tiến trình**:
nó *"chạy mỗi thread đến khi bị block bởi syscall"* và *"model CPU như thể nhanh vô hạn"*, nên **mọi
bug concurrency trong recovery layer của bạn là vô hình**; (3) không có hook invariant in-process,
mà đó là chỗ phần lớn oracle của TigerBeetle sống; (4) không có artifact replay ở mức event, không
shrinking; (5) **không inject được storage corruption**: không torn write, không bit flip (đúng lớp
fault đã làm VOPR của TigerBeetle mất mặt); (6) lỗ hổng ghi rõ trong tài liệu mà **đặc biệt cắn một
pipeline market-data**: không IPv6, không `sendfile`, **không `SO_REUSEADDR`**, không `TCP_FASTOPEN`,
`vfork` implement như đồng nghĩa của `fork`, và **busy-loop làm deadlock cả simulation**.

**Chỗ nó xứng đáng có mặt:** như một **harness thứ hai, bổ trợ**, để test feed handler của bên thứ
ba **như những tiến trình nguyên vẹn** trên một stack TCP/UDP thật, tức là nửa mang hình dạng
Jepsen trong chiến lược của bạn. Hãy đóng khung nó như vậy, **đừng** nói "arguably repurposable
rather than rebuilt" như bản dossier cũ.

**Lựa chọn kế cận, cũng đã xác minh:** `facebookexperimental/hermit` (1.392★) gần hơn với thứ bạn
muốn: kiểm soát *"thread scheduling, time, random data, kết quả CPUID, và metadata file được
chọn"* qua ptrace/Reverie, nhưng README nói **"Hermit đang ở maintenance mode"**, chỉ x86-64 Linux,
và trỏ cài đặt về một fork cá nhân. `rr` (10.602★) hỗ trợ *microarchitecture* Apple Silicon M-series
nhưng **chỉ dưới Linux có PMU virtualization**, mà phần lớn cloud VM không expose, và rr replay
**một execution đã quan sát** trong khi DST **explore nhiều**: công cụ khác loại.

### 11.11 Demo đã build và đo được

Tất cả nằm ở scratchpad, build bằng Apple clang 21 / libc++ 210106 trên M2 Max arm64:

| File | Chứng minh | Số đo |
|---|---|---|
| `det3.cpp` | ⭐ **Cái quyết định.** Với arena tất định, hash-by-pointer **vẫn** nondeterministic qua 4 run; hash-by-ID hoàn toàn ổn định | xem §11.6 |
| `trampoline.cpp` | Sự cần thiết của symmetric transfer | naive **exit 139** ở 400k/700k/1M, **OK ở 100k**; symmetric OK ở 1M |
| `arena_coro.cpp` | `promise_type::operator new` **có** bắt được việc cấp phát frame; layout frame **không** ABI-stable qua các mức `-O` | 16 frame cả hai mức, **704 B ở `-O0` vs 608 B ở `-O2`** |
| `allocbench.cpp` | Arena vs global `new`; và **HALO elision là thật nhưng không đoán được** | **2,9 ns vs 17,8 ns (6,1×)**. Bản đầu bị elide hoàn toàn ở `-O2` (`operator new` không hề chạy) |
| `bench.cpp` | Throughput executor virtual-clock và mô hình mật độ | **15–29M resume/giây**; 0,4× / 46× / 4418× |
| `shrink.cpp` | PoC đầu-cuối: một `ChoiceSource` với mode Generate/Replay, shrinker shortlex trên một feed handler đồ chơi có bug cài sẵn | tape **14 → 4** choice trong **43 lần chạy lại**; counterexample tối thiểu `[2,1,1,1]` = snapshot-start, gap, gap, gap, **cái tape đã shrink CHÍNH LÀ bug report** |
| `fp2.cpp` | FP contraction ở mức instruction | `off` → `fmul`; `on` (mặc định) và `fast` → `fmadd` |
| `det.cpp`, `det2.cpp` | `hash<string>` ổn định qua các process; `set<Node*>` trên heap pointer nondeterministic **và không nhất quán** | run 3 tình cờ ra đúng thứ tự: ca nguy hiểm |

Caveat của `shrink.cpp`: nửa thí nghiệm về seed-perturbation bị suy biến (bug đồ chơi quá dễ chạm,
nên 20/20 seed liền kề cũng fail). Lập luận "một seed không có gradient" là **lập luận cấu trúc**:
đổi seed sẽ re-randomize mọi draw phía sau: chứ không phải điều mà bản đồ chơi chứng minh. Nửa
tape-shrinking thì vững, và đó mới là điểm chính.
