# Chuỗi lập luận: từ nghiên cứu đến quyết định chọn dự án

Tài liệu này giải thích **tại sao** đi tới kết luận trong `RESEARCH-DOSSIER.md`, theo từng bước
logic. Mỗi bước là một mắt xích: bước sau chỉ đúng nếu bước trước đúng.

Ngày nghiên cứu: 2026-07-29. Ràng buộc đã biết: **chỉ có cloud VM**, thời gian mở.

---

## Bước 1 — Đặt lại câu hỏi cho đúng

Câu hỏi ban đầu: *"ai cũng làm order book, làm sao để khác biệt?"*

Câu hỏi này ngầm giả định rằng vấn đề là **chọn chủ đề lạ hơn**. Giả định đó sai, và việc nhận ra
nó sai là mắt xích quan trọng nhất của toàn bộ phân tích. Nhưng chưa thể chứng minh ngay — cần dữ
liệu trước. Nên bước đầu tiên chỉ là: **đo mức độ bão hòa bằng số, không bằng cảm giác.**

---

## Bước 2 — Đo độ bão hòa

Đếm bằng GitHub API (có xác thực), không dùng snippet tìm kiếm:

| Truy vấn | Số repo |
|---|---|
| `"order book" language:C++ created:>2026-01-01` | **1.071** |
| …trong đó có **≥5 sao** | **7** (0,65%) |
| `"matching engine" language:C++ created:>2026-01-01` | **748** |
| `itch nasdaq` trong C++ | **96** (~70% push trong 6 tháng gần nhất, 0 sao) |

**1.071 order book viết bằng C++ trong bảy tháng.** Đây không còn là "hơi đông" — đây là bão hòa
tới mức một repo mới về cơ bản là vô hình.

---

## Bước 3 — Đo phía đối lập, và đây là chỗ phát hiện ra điều bất thường

Cùng API, cùng ngày, nhưng đếm những thứ *khác*:

| Truy vấn | Số repo |
|---|---|
| `"gap fill" multicast market data` | **0** |
| `glimpse soupbintcp` | **0** |
| `"OUCH protocol nasdaq"` | **0** |
| `mock exchange gateway trading` | **0** |
| `"AF_XDP" multicast` | **0** |
| `deterministic scheduler test language:C++` | **0** |
| `feed arbitration multicast market data` | **1** (0 sao) |
| `deterministic replay trading system` | **7** (cái tốt nhất: 1 sao) |

**Suy ra được quy luật:** mọi thứ **giải mã** một feed hoặc **khớp** một lệnh thì bão hòa. Mọi thứ
mô hình hóa **phía đối tác** — đường phục hồi dữ liệu, hành vi của sàn đối với lệnh của bạn, vị trí
trong hàng đợi, đồng hồ — thì trống rỗng.

Nói cách khác: ai cũng xây **đầu vào** (ingress). Gần như không ai xây **đầu ra** (egress) — mà
egress mới là nơi chứa các bài toán đúng/sai khó: đánh số thứ tự phiên, phát lại khi kết nối lại,
trạng thái lệnh đang bay lúc mất kết nối, cancel-on-disconnect.

---

## Bước 4 — Tại sao order book thất bại (giờ đã chứng minh được)

Bây giờ mới trả lời được Bước 1. Order book thất bại **không phải vì nó dễ**, mà vì **nó không kèm
theo một tuyên bố có thể kiểm chứng** (falsifiable claim). Nó là một hiện vật mà người review không
thể đánh giá chất lượng trong năm phút.

Bằng chứng từ chính người trong ngành:
- *"Tôi sẽ không đem nó ra 'trưng' trong CV"* — TeamBlind, 2021
- *"Về bản chất là một dict Python với list cho mỗi mức giá. Làm trong 10 phút."*
- *"Có video YouTube nào đang lan truyền à? Dạo này sinh viên mới ra trường đua nhau show benchmark
  matching engine"* — r/highfreqtrading, 2026-01-16

**Kết luận của bước này — đây là trục của toàn bộ chiến lược:**

> Đơn vị của sự khác biệt là **một tuyên bố có thể kiểm chứng**, không phải một codebase.

So sánh hai câu:
- *"Tôi làm order book low-latency bằng C++, 2,5 tỷ TPS."* → không kiểm chứng được, và bị châm biếm.
- *"Đây là một kịch bản lỗi tất định khiến sáu ITCH feed handler đã công bố âm thầm dựng ra order
  book sai, tái hiện được từ seed 4711 bằng một câu lệnh."* → kiểm chứng được trong một phút.

---

## Bước 5 — Kiểm tra: liệu vùng bão hòa có ít nhất là *đúng* không?

Nếu 1.071 repo kia chất lượng tốt, thì "bão hòa" nghĩa là bài toán đã giải xong. Cần kiểm tra.

[flash1-dev/matching-engine-benchmark](https://github.com/flash1-dev/matching-engine-benchmark) đã
chạy **247 matching engine** qua một oracle đồng thuận:

- **47 cái đúng ngay khi ship**
- 110 cái chỉ đúng sau khi sửa
- **87 cái sai lệch, crash, hoặc không chạy được**
- **181 issue đã gửi lên upstream, 18 cái được sửa, không cái nào bị từ chối**

Lỗi trong chính các repo *nổi tiếng*: bản 311 sao có use-after-free khi cancel; bản 266 sao phá vỡ
thứ tự ưu tiên FIFO; bản 387 sao có lỗi xóa RB-tree. Lớp lỗi phổ biến nhất, tìm thấy độc lập ở mười
engine: **khớp lệnh theo giá của bên chủ động thay vì giá của bên đặt lệnh chờ** — đây không phải chi
tiết tối ưu, đây là *định nghĩa* của một limit order book.

**Hai suy luận từ bước này:**
1. Vùng bão hòa không chỉ đông, mà còn **đầy code sai**. Thêm một cái nữa không có giá trị.
2. Quan trọng hơn: một hiện vật **kiểm thử vi phân nghiêm túc** trong lĩnh vực này đã tạo ra 181
   issue và 18 bản sửa upstream. Tức là **dạng sản phẩm đó có tác dụng thật**. Đây là bằng chứng
   mạnh nhất về hướng đi, và nó sẽ được dùng lại ở Bước 9.

(Lưu ý trung thực: Flash One là công ty kinh doanh bản quyền phát minh và tự báo engine của mình
nhanh nhất — có xung đột lợi ích. Nhưng oracle đúng/sai được thiết lập từ ba engine độc lập của bên
thứ ba, và mọi phát hiện đều kiểm tra được trong source. Phương pháp đứng vững độc lập với lợi ích
thương mại của họ.)

---

## Bước 6 — Kiểm tra phía cầu: nhà tuyển dụng thực sự đánh giá điều gì?

Không thể chỉ dựa vào phía cung (repo có gì). Cần biết phía cầu thưởng cho cái gì.

Chủ đề duy nhất xuất hiện ở **cả hai phía** của bảng cân đối — vừa là điều người tuyển dụng sàng lọc,
vừa là điều người hành nghề đau khổ — là **phương pháp đo lường**:

- *"Nếu buổi phỏng vấn không hề yêu cầu ứng viên đo latency, thì bạn không thực sự tuyển cho HFT."*
- Cờ đỏ được nêu thẳng: *"Đo throughput nhưng bỏ qua jitter và tail latency."*
- Mọi hãng đều tự xây tracer riêng: Optiver có *Tarantula*, Jane Street có *magic-trace*, Rigtorp
  viết *hiccups*.
- Carl Cook (Optiver), slide CppCon 2017 — câu "không công cụ nào làm được" mạnh nhất tìm thấy:
  sampling profiler *"bỏ sót đúng những sự kiện quan trọng"*; instrumentation profiler *"quá xâm
  lấn"*; microbenchmark *"không đại diện cho môi trường thực tế"*.

Đồng thời, phía cầu nói rõ những gì **không** gây ấn tượng: chiến lược giao dịch cá nhân (*"chúng tôi
có lẽ sẽ không tin bạn"* — HRT), bot crypto (*"ôi cưng giỏi quá"* — Optiver, lịch sự nhưng bằng
không), và đặc biệt: **repo có mùi LLM là phản tác dụng** — một CTO của hãng HFT đã công khai nói
thẳng điều đó về một repo trên HN.

---

## Bước 7 — Bốn luồng nghiên cứu hội tụ… rồi ràng buộc phần cứng phá vỡ kết luận

Bốn luồng nghiên cứu được giao đề bài **khác nhau có chủ đích** đều tự đi tới cùng một chỗ:

| Luồng | Kết luận độc lập |
|---|---|
| Phía cung OSS | Thiếu **hạ tầng đo lường**: không có C++ HdrHistogram nào được bảo trì; google/benchmark không báo percentile |
| Nỗi đau người hành nghề | 7 trong 16 điểm đau xếp hạng đầu đều là **một** vấn đề: đo tail latency đáng tin, quy được nguyên nhân, không làm nhiễu |
| Phía cầu | *"Nếu phỏng vấn không hỏi cách đo latency thì không phải tuyển cho HFT"* |
| Khoảng trống công cụ | Khoảng trống #1 = quy nguyên nhân outlier theo từng request. Mọi nguyên liệu đã chín; không ai lắp lại |

Sự hội tụ này **không phải trùng hợp** — nó có nguyên nhân cấu trúc: công cụ đo lường (a) ai cũng
cần, (b) không phải lợi thế cạnh tranh cần giữ kín như một alpha, nhưng (c) luôn phải tự xây vì
không có bản mã nguồn mở. Đó chính là hình dạng của một **thất bại thị trường** — và thất bại thị
trường là nơi một người giỏi làm ra được thứ người khác *dùng thật*.

**Nên đó *đã là* đề xuất của tôi — cho tới khi biết ràng buộc phần cứng.**

### Vì sao ràng buộc "chỉ có cloud VM" phá vỡ kết luận đó

Đây là mắt xích mà tôi phải tự bác bỏ chính mình:

1. **Bạn không thể trả lời phản biện tiêu chuẩn.** Lời chỉnh sửa phổ biến nhất dành cho bất kỳ ai
   công bố một con số latency là: *"anh dùng software timestamp, không phải hardware timestamp
   wire-to-wire."* vNIC trên cloud **không có** hardware timestamping. Bạn sẽ công bố đúng loại con
   số bị châm biếm, mà không có cách nào tự bảo vệ.
2. **Nền tảng đo lường vắng mặt trên cloud ở mọi trục:** không PMC (nên không `rdpmc`, không PEBS),
   không Intel PT (nên không có flight recorder), `cpuid` bị trap, clocksource ảo chậm, không flow
   steering, và trên Graviton thì không điều khiển được C-state hay tần số. Lời một người hành nghề:
   *"đo dưới micro giây trên cloud VM thì loạn hết, nên người ta không dùng nó cho việc gì quan
   trọng."*
3. **Một công cụ đo cần có tải để đo**, mà tải chính là cái thứ đang bão hòa. Bản thân cái rig không
   phải một dự án.
4. **"Tôi làm benchmark tốt hơn" có hình dạng của một công cụ.** Người phỏng vấn thưởng cho *phát
   hiện*, và phát hiện thú vị ở vùng đó đòi phần cứng đáng tin.

---

## Bước 8 — Ràng buộc thực ra làm sáng tỏ, không phải giới hạn

Đây là bước đảo chiều quan trọng.

Ràng buộc chỉ-có-cloud loại bỏ đúng cái lớp *"đo nanosecond trên bare metal đã tune"* — mà lớp đó **vừa**
đầy tuyên bố không kiểm chứng được, **vừa** không thể làm tử tế nếu thiếu phần cứng bạn không có. Bỏ
nó đi không mất gì.

Còn lại là gì? Chính là thứ mà bản khảo sát xếp hạng **trống nhất và có địa vị cao nhất**:

> *"Tín hiệu mạnh nhất là hạ tầng kiểm thử và phục hồi, không phải tốc độ hot path. Đây là những
> thứ một hãng chắc chắn đã tự xây bên trong, chắc chắn không chia sẻ, và sẽ nhận ra ngay là việc
> của người đã từng ship hàng — chứ không phải người chỉ mới đọc."*

Và mấu chốt kỹ thuật: **tính đúng đắn, phục hồi, và tính tất định là độc lập với phần cứng theo
thiết kế** — vì tính tất định được **mô phỏng**, không phải **đo**. Một cloud VM là chỗ hoàn toàn
chính đáng để làm toàn bộ việc này.

---

## Bước 9 — Luận đề: xây phía đối tác (build the counterparty)

> **Một stack C++ mô phỏng sàn giao dịch tất định + phục hồi dữ liệu thị trường.** Xây cái *hành xử
> như một sàn*, và cái *sống sót được khi sàn hành xử tồi* — với một bộ mô phỏng có seed khiến cả hai
> tái hiện được.

Ba thành phần ghép được với nhau. Mỗi cái là một khoảng trống đã kiểm chứng độc lập. Không cái nào
cần phần cứng đặc biệt.

**Thành phần 1 — Sàn giả nói đúng giao thức thật.** ITCH qua multicast + OUCH/SoupBinTCP qua TCP,
khớp lệnh theo price-time priority, tiêm độ trễ từng chặng. Nhắc lại Bước 3: decoder thì bão hòa,
còn **encoder hành xử như một sàn thì gần như bằng không**. Thư viện OUCH C++ duy nhất có 8 sao và
chết 11 năm; bản SoupBinTCP C++ tốt nhất đang tồn tại có **ba sao**.

**Thành phần 2 — Lớp phục hồi.** Phát hiện gap, rewind/retransmit MoldUDP64, snapshot kiểu Glimpse,
**phân xử đường A/B**, chặn NAK dồn dập. Xếp **#1** theo mức độ một hãng thực sự quan tâm. Các hãng
trả Informatica sáu chữ số mỗi năm phần lớn là cho đúng thứ này; Aeron (8.765 sao) cho bạn transport
tin cậy và **không có** ngữ nghĩa phục hồi market-data nào.

**Thành phần 3 — Bộ tiêm lỗi tất định.** Có seed, hiểu giao thức: mất gói, đảo thứ tự, trùng lặp,
lệch A/B, reset sequence, đua giữa snapshot và incremental. Ghi lại được, phát lại từ seed.
`tcpreplay` mù giao thức — nó không thể ghi lại số thứ tự MoldUDP64 hay phân xử A/B. Công cụ thực tế
mọi người đang dùng, `rigtorp/udpreplay`, **nặng 34 KB và chết từ 2023**.

### Vì sao ba cái ghép lại lớn hơn tổng các phần

Sàn giả sinh feed → bộ tiêm lỗi làm hỏng nó một cách tất định → lớp phục hồi phải sống sót → toàn bộ
phát lại được từ một seed. Đó là **VOPR của TigerBeetle áp vào kết nối sàn giao dịch** — và nó chạy
trọn vẹn trên một cloud VM.

### Khoảng trống lớn nhất trùng đúng sở trường bạn muốn

`deterministic scheduler test language:C++` → **0 repo**. Cụ thể hơn:

| Repo | Sao | Push cuối | Trạng thái |
|---|---|---|---|
| `microsoft/cpp-systematic-testing` | 44 | 2022-09-29 | **đã archive**, tổng cộng 2 commit |
| `microsoft/coyote` | 1.589 | 2024-12-11 | ngủ đông 19 tháng |
| `mpdn/unthread` | 48 | 2023-05-07 | chết; một lõi, không preempt, không `std::thread` |
| `tokio-rs/loom` | 2.767 | 2026-02-20 | sống (Rust) |
| `awslabs/shuttle` | 1.036 | 2026-07-28 | sống (Rust) |
| `madsim-rs/madsim` | 1.139 | 2026-02-16 | sống (Rust) |
| `tokio-rs/turmoil` | 1.230 | 2026-07-21 | sống (Rust) |

Rust có bốn lựa chọn khỏe mạnh; JVM có Lincheck và Fray. **C++ có một shim pthread 48 sao đã chết** —
và đó là mục *duy nhất* trong phần C/C++ của danh sách awesome-deterministic-simulation-testing.

Hai điều biến đây thành **cơ hội về thời điểm** chứ không phải khoảng trống vĩnh viễn:
1. Flow của FoundationDB **là** C++, nhưng là một biến thể ngôn ngữ riêng với compiler actor riêng,
   không tách ra được. Đó là lý do lịch sử vì sao C++ không có bản tương đương.
2. **C++20 coroutine đã xóa bỏ rào cản đó.** Cánh cửa đã mở và chưa ai bước qua.

Bổ trợ: **TSan không thấy được weak memory.** Cờ `force_seq_cst_atomics` của chính Clang là lời thừa
nhận — TSan quan sát một lần chạy thật trên x86-TSO, và x86-TSO *che đi* đúng những phép đảo thứ tự
mà lập luận `relaxed`/`acquire` của bạn phụ thuộc vào. Nó sẽ cho pass một queue rồi queue đó vỡ trên
Graviton, và phần Limitations của nó không hề nói vậy. Phần lớn người dùng tin rằng TSan xanh nghĩa
là đúng.

### Tuyên bố có thể kiểm chứng mà luận đề này sinh ra

> *"87 trong 247 matching engine mã nguồn mở đã sai so với một oracle đúng đắn. Chưa ai kiểm thử
> **đường phục hồi**, vì không có công cụ nào để kiểm thử. Đây là công cụ. Và đây là kịch bản lỗi tất
> định khiến từng feed handler ITCH đã công bố âm thầm dựng ra order book sai."*

Chú ý mắt xích quay lại **Bước 5**: đây chính xác là phương pháp của flash1-dev, áp vào đúng lớp mà
họ *không* kiểm thử. Và nghiên cứu đó đã tạo 181 issue với 18 bản sửa upstream. Đó là bằng chứng
rằng hình dạng hiện vật này có tác dụng với maintainer — proxy tốt nhất hiện có cho việc nó có tác
dụng với người phỏng vấn.

---

## Bước 10 — Hai thứ tôi tự kiểm chứng, không phải nghe lại

Nguyên tắc: không xây luận đề trên lời khẳng định của một subagent.

**Kiểm chứng 1 — một phát hiện thật, nằm ngay trước mắt.** README của `rigtorp/MPMCQueue` (1.555
sao) đánh dấu `- [X] Add allocator supports so that the queue could be used with huge pages and
shared memory` là **đã xong**. Dòng 278 của header là `Slot<T> *slots_;` — một con trỏ thô, nên khối
điều khiển **không position-independent**: tiến trình thứ hai map nó ở địa chỉ khác là vỡ. `grep` cả
header tìm "shared memory|shm|interprocess|process" → **không khớp gì**. Không có một cảnh báo nào.

→ Đây là bài blog ~1 ngày kèm chương trình tái hiện 20 dòng. Không phải một dự án, nhưng là uy tín
miễn phí, và nó thể hiện đúng thói quen: **kiểm tra lời khẳng định**.

**Kiểm chứng 2 — sửa lại chính subagent của mình.** Một luồng báo rằng
`microsoft/cpp-systematic-testing` bị archive ngày 2026-06-11. Nó *có* bị archive, nhưng push cuối
là **2022-09-29** — chết bốn năm, tổng cộng 2 commit. Kết luận không đổi, nhưng con số thì phải đúng.

---

## Bước 11 — Đánh đổi còn lại, nói thẳng

Luận đề này **đổi nanosecond lấy tính đúng đắn**. Với ràng buộc chỉ-cloud thì đánh đổi đó là bắt
buộc, và theo Bước 6/8 nó rơi vào nửa có địa vị cao hơn của lĩnh vực. Nhưng nếu mục tiêu cụ thể là
một ghế tối ưu hot path, thì cuối cùng vẫn cần bare metal — khi đó thứ nên mua là một workstation
Intel Skylake+ cũ có quyền vào BIOS, kèm card Xilinx/Solarflare X2522 (header ef_vi là BSD-2). Không
cần cho bất kỳ phần nào ở Bước 9.

**Nếu muốn gần phần cứng hơn ngay từ đầu:** dự án kernel-bypass duy nhất sống sót được với cloud-only
là một **AF_XDP multicast receiver** (`AF_XDP multicast` → 0 repo) — kiểm thử được trên `veth` +
`XDP_SKB`, không cần NIC đặc biệt. Nó gắn vào như Thành phần 0. Bẫy cần biết: API socket AF_XDP đã
**bị bỏ khỏi libbpf** sang libxdp, mà gần như mọi tutorial và mọi repo 0 sao vẫn còn
`#include <bpf/xsk.h>`.

---

## Bước 12 — Nguyên tắc xuyên suốt: bài viết mới là tài sản, không phải repo

- Câu chuyện thành công chi tiết duy nhất trong toàn bộ tư liệu — 4 năm kinh nghiệm cloud/front-end
  *không liên quan* → phỏng vấn ở nhiều hãng lớn — mấu chốt là: *"Tôi ghi log tiến độ trong lúc làm
  dự án, và đưa hết lên 2 repo GitHub."*
- Mọi tuyên bố về latency hay tính đúng đắn khi công bố đều kích hoạt phản biện về **phương pháp**,
  chưa bao giờ về cách hiện thực. Nên đi trước nó: công bố phương pháp, công bố giới hạn, một câu
  lệnh để tái hiện, kèm seed.
- Chín trong khoảng một tá nhà tài trợ CppCon là các hãng giao dịch. Một lightning talk hoặc một bài
  viết tử tế về một phát hiện thật là con đường vào trực tiếp hơn bất kỳ dòng nào trong CV.
- Hiện vật đi được trong kênh đó trông giống `hiccups` 136 sao của Rigtorp, **không** giống bất cứ
  thứ gì có chữ "ultra-low-latency trading engine" trong tiêu đề.
