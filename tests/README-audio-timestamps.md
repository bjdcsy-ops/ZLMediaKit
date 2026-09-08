# G711 与 AAC 音频时钟回归验证

## 最终候选与验证入口（2026-09-08）

当前代码与最终 ARM64 v2 产物见
[SR 配对与低延迟完整报告](REPORT-ipc-sr-pairing-low-latency-20260908.md)。
最终候选包含 G711 样本时钟、AAC 批次边界、RTP 回环精度和 RTSP 全轨共同 NTP
偏移修复；实际站点配置保持 `modify_stamp=0`，生产未切换。
最终 20 项公开输入路径测试及 G711/AAC/RTP-NTP 回归通过。四台当前格式主对照及
最终产物双头补测保留全部原始失败和补充验收边界，不等同于 NVR 显示/听音验收。

下列各阶段按当时状态保留。旧三文件候选、旧产物哈希及旧失败要求不是最终候选状态；
`--verify-rtp-wrap` 已通过并纳入默认测试，`--verify-sr-reanchor` 原始门槛仍单独保留。

## 历史：本机配置验证与三项小修（2026-09-08 后续）

最新状态见 [本机配置冷启动与小修报告](REPORT-ipc-coldstart-minimal-fixes-20260908.md)。
当前使用 1.203 实际有效配置 `modify_stamp=0`，没有切换上游默认值，参数以后仍可修改。
G711 96 组、AAC 17 组（5252 加入位置）、RTSP 音频 14 组、NTP/回环 12 组 ARM64
回归通过；五采样率回环最大误差 0 ms。独立 `--verify-sr-reanchor` 仍失败，
没有通过过滤失败、修改配置值或改变 SR 语义来获得通过。

后文三文件候选及 `--verify-rtp-wrap` 失败记录是三项小修前的历史基线。
当前 `--verify-rtp-wrap` 已通过并纳入默认 NTP 测试，原始 SR 失败门槛继续单独运行。

## 历史：三文件最小候选实施与验证（2026-09-08）

当时工作树采用 [最小兼容设计](DESIGN-ipc-audio-compatibility.md)，已实施、已构建，
**未替换 1.203 生产程序**。本节描述该历史候选；后文的 12:34 部署记录属于更早版本，
其中全局 NtpStamp 平滑、RtpReceiver 扩展及 AAC raw metadata 已从候选撤出。

随后完成四台 IPC 的 46 组编码组合与 1 组 90 秒验证，详见
[四摄像头实机报告](REPORT-ipc-codec-matrix-20260908.md)。G711/AAC 主矩阵 28/28
均有三段音视频，但仍存在时间戳非递增，当前候选不满足生产替换条件。

| 范围 | 该历史阶段状态 |
| --- | --- |
| ZLM 相对 HEAD `07253110` | 3 个运行时文件，+30/−3：G711 批次规则与首 RTP=0 的 muxer 初始化 |
| ZLM 相对官方 `296dcebe` | 21 个运行时文件，+737/−158；其中音频及 RTP 辅助为12文件 +525/−75，其余为已有 HTTP 修复 |
| 公共时钟 | Stamp、RtpReceiver、RtspPlayer 与官方逐字一致；RtspSession 保留已有 HTTP Date 修复 |
| AAC | 保留已提交的 AU/聚合/分片修复；恢复普通 Frame/DTS，不再透传源 RTP 绝对起点 |
| FFmpeg | 隔离源码恢复原 rtcp_from_packet；仅追加 AAC `max_delay=0` 即时发送条件，运行时1文件 +3/−1，另3文件为测试 |

FFmpeg AAC 即时发送补丁在9月4日临时工作树验证过，但当时未提交/未部署；此前203
两个版本均未包含它。本次从原始记录恢复并重新执行真实 mux/FATE，不能将历史测试
误写为现场已经具备该行为。候选不增加G711下一包等待；FFmpeg的即时发送消除包化
阶段额外等下一AU的缓存，仍有AAC编码帧积累。没有新增媒体排队，也未测量端到端
P50/P95或完成NVR听音/录像验收。

| 本次验证 | 实际结果 |
| --- | --- |
| ARM64 G711专项 | 89组通过；940包批次fixture的所有加入位置、样本/payload守恒及边界用例保留 |
| G711/视频与AAC包结构 | 14组通过 |
| AAC普通DTS兼容 | 9组通过；16/44.1/48 kHz各2000AU、保留muxer换源、VOD、ADTS、FrameStamp、首RTP=0 |
| 官方RTP/NTP兼容 | 9组通过；合法SR校准、同SSRC小幅重启、先SR后换SSRC、B帧和序号回环 |
| ARM64合成G711输入 | PCMA16k批次与PCMU32k连续抖动各20秒通过；样本间断/回退/稳定态告警0 |
| ARM64合成完整中继 | 候选ZLM＋候选FFmpeg，G711→AAC两段各25秒通过；音频检查及稳定态告警0 |
| FFmpeg真实mux/FATE | 单AU/分片/尾AU即时发送、正延迟聚合通过；纯1033a46相同测试3项失败 |
| FFmpeg原SR语义 | 默认关闭、skip、±100ppm模拟1小时、停供10秒恢复、PTS前跳通过；突发仍有SR相位噪声 |
| 203真实G711源，60秒 | 2901音频包，样本间断/回退/相关告警0；视频/SR检查通过 |
| 203真实G711→AAC，60秒 | AAC检查通过，6次PLAY RTP-Info检查通过；视频有一次−7ms步进，**整体失败**，即使相关告警0 |
| 原始SR回放 `--verify-sr-reanchor` | **失败，exit 1**：视频40→−59ms、AAC约64→−166ms、完整AU 64→0ms |
| 官方RTP回环精度 `--verify-rtp-wrap` | **失败，exit 1**：16k固定用例回环后+12ms；官方与候选逐值一致，本批未修 |

默认兼容性检查与两项独立失败门槛分开报告；未删除原始捕获数据，也未放宽精度要求。
SR回放经过真实receiver、decoder、Track、muxer及SR序列化，payload守恒仍不能替代
时间间隔正确性。默认稳定SR对照明确是合成数据，不代替原始现场回放。

```bash
# 已配置的Linux ARM64构建环境
cmake -S . -B .build/linux-arm64/zlmediakit -DENABLE_TESTS=ON
cmake --build .build/linux-arm64/zlmediakit --target \
  test_g711_rtp test_rtsp_audio_clock test_aac_rtp_clock test_rtp_ntp_clock -j6
release/linux/Release/test_g711_rtp
release/linux/Release/test_rtsp_audio_clock
release/linux/Release/test_aac_rtp_clock
release/linux/Release/test_rtp_ntp_clock

# 该历史阶段两条均失败；最终候选的 wrap 已通过，SR 原门槛仍单独记录
release/linux/Release/test_rtp_ntp_clock --verify-sr-reanchor
release/linux/Release/test_rtp_ntp_clock --verify-rtp-wrap
```

实机候选身份：ZLM SHA-256 `f7f2678bd55bf936093d1e747d341ea203724fbe881089df6a3aaa3d75434b98`；
FFmpeg `1033a46-dirty.23734da`，SHA-256
`dcfbccc6c21f6d6d9c79e1593bd56453d28fd0f2ad7b7769921fcc43ac4c5759`。
两轮使用 `/tmp/zlm-minimal-validation-20260908` 的临时程序、回环端口和关闭hook的配置，
结束后停止测试进程并删除临时配置；生产两份程序及ZLM配置校验值不变，未重启生产服务。
G711→AAC实机测试的视频使用copy，未据此宣称RKMPP性能或长期兼容性已验收。

原队列SR问题未关闭，候选仅作为最小codec基线保留。推迟应用SR、只禁止时间回退或
缓存同戳包都不能完整解决既有反例并保留官方校时/重启语义；本批不再加入公共时钟策略。
完整源码分类见 [上游差异记录](REVIEW-upstream-audio-delta.md)。

## 范围

本次修复针对 RTSP 重打包路径（`rtsp.directProxy=0`）。保留上游明确声明的
G711 采样率和声道数，不修改上游设备、不禁用 RTCP、不改写通用 Stamp
异常处理，也不依赖 FFmpeg 滤镜掩盖输入问题。

| 场景 | 输出约束 |
| --- | --- |
| PCMU/8000/1、PCMA/8000/1 | 分别保留静态 PT0、PT8 |
| 非标准 PT0 + 显式 PCMU/16000/1 | RTSP 输出动态 PT，SDP、RTP clock 和样本数均为真实规格 |
| 16 kHz 单声道、40 ms/640B 输入 | 默认重打包为20 ms/320B；每秒50包，不复制样本 |
| 小 MTU、多声道 | 不切断交错样本；RTP timestamp 按每声道样本数递增 |
| G711 单包输入 | 当次解包输出，不等下一包才能交给下游 |
| AAC 单 AU/分片 | 使用当前 AU 首片时间戳，不使用前一包的时间戳 |
| AAC 多 AU | 本轮验证 AAC-hbr、AAC-LC、1024 samples/AU |

## 现场根因与修复边界

2026-09-04 对 RK3588-203 通道4的20秒被动抓包发现：

1. 相机正常发送500个640B PCMU包，音频实际为16 kHz，RTP前进19.96秒。
2. 旧 G711Track 强制8 kHz，重打包成2000个160B包，出现499次时间回退。
   上下游都是320000B，错误是样本率与时间区间，而非重复 payload。
3. ZLM共享音视频同步时钟被回退处理推进到约2倍；private-input视频RTP仍为1倍，
   但相邻SR的NTP约前进10秒、RTP仅前进5秒。
4. FFmpeg收到这样的SR后，live视频RTP每约5秒跳约5秒；整数值与Stamp警告吻合。
   AAC在20秒墙钟内产生约39.936秒媒体时间。

此前的 `+rtcp_from_packet` 仍在部署程序和实际命令中。本次不是删除或重做该修复。
修正G711轨道与分包参数后，无需修改通用Stamp即可恢复上述同步关系。

FFmpeg配套候选补丁独立处理 `max_delay=0` 的AAC RTP包化：当前AU立即发送，
不等下一AU触发缓存输出。16k/1024samples对应64ms媒体帧长；这不是承诺端到端
视频延迟必然减少64ms，也不消除AAC编码本身的帧积累和priming。

## 自动回归

先构建项目测试目标，再按顺序运行：

```bash
cmake --build build-audio --target test_g711_rtp test_rtsp_audio_clock MediaServer --parallel 6
release/linux/Debug/test_g711_rtp
release/linux/Debug/test_rtsp_audio_clock

# 生成仅用于测试的25帧HEVC素材，不连接任何真实IPC。
ffmpeg -f lavfi -i testsrc2=size=128x96:rate=25 -t 1 -c:v libx265 \
  -x265-params aud=1:repeat-headers=1:keyint=25:bframes=0 -an -f hevc fixture.h265

# 在临时配置和回环端口启动/清理MediaServer及模拟相机。
python3 tests/test_rtsp_g711_sender_clock.py \
  --server release/linux/Debug/MediaServer --fixture fixture.h265

# 可选完整AAC中继：FFmpeg需支持rtcp_from_packet、PCMU解码与AAC编码。
# 视频copy用于隔离音频/同步机制，不代替RKMPP硬件编码或NVR验收。
python3 tests/test_rtsp_g711_sender_clock.py \
  --server release/linux/Debug/MediaServer --fixture fixture.h265 \
  --ffmpeg /absolute/path/to/test-ffmpeg
```

| 测试 | 必须检查的结果 |
| --- | --- |
| `test_g711_rtp` | PCMA/U、静态/动态PT、参数/WAVE元数据、字节守恒、MTU、flush、时间间断、RTP回绕、RawEncoder约束 |
| `test_rtsp_audio_clock` | 20秒视频RTP与SR均约20秒；AAC当前AU、聚合、分片、丢包、序号重置和ADTS兼容 |
| Python RTSP集成 | 动态PT/16k SDP、音频无回退、样本/墙钟约16000、SR NTP/RTP增量相符 |
| 可选FFmpeg中继 | live音视频RTP均按1倍墙钟推进，无周期性秒级跳变 |

模拟相机启动时，首次SR可能替换按到包时间建立的临时锚点。集成测试单独保留
首秒的 `startup_timing_warnings`，之后的周期性/稳定态异常计入
`timing_warnings` 并使测试失败；不通过丢弃全部日志来消除告警。

## 本次验证记录（2026-09-04）

| 项目 | 基线 | 修复版 |
| --- | --- | --- |
| 20秒G711/视频内存时钟测试 | 视频RTP19.96s、SR39.80s、音频499次非递增 | 两者均19.96s、音频0次非递增 |
| G711专项 | 首批14项失败 | 23组通过 |
| 音视频时钟与AAC专项 | G711时钟、AAC单AU/聚合失败 | 12组通过 |
| FFmpeg AAC RTP FATE | 即时发送/SR对应AU/大AU后小AU失败 | 通过；正延迟聚合、分片、末包行为保持 |
| FFmpeg相关FATE、ASan/UBSan专项 | — | 通过 |
| 模拟相机→ZLM→同一FFmpeg→ZLM（约42秒） | 稳定复现4次约每5秒的Stamp警告、SR约2倍 | exit 0；private/live均无音频回退，稳定态时钟警告0条 |

整链修复后live视频在17.9997秒墙钟内前进17.999秒，AAC在17.9189秒内前进
17.873秒；视频SR的NTP/RTP均前进15.079秒，音频SR均前进15.184秒。
首秒单独记录一次G711初次SR锚定提示，不计为周期性异常；之后没有新增时钟警告。
测试视频为copy，且仅运行约42秒，不据此宣称RKMPP性能、长期稳定性或NVR兼容已验收。

## G711 短暂时间戳抖动补充修复

首版解决采样率误判后，32 kHz PCMA 的现场输入仍出现每包40 ms音频、
RTP时间戳间隔成对30/50 ms的情况。包序号连续，不能将其直接解释为缺包。
首版在偏差超过1 ms时flush并重新定位，600字节MTU下可使尾包之后的时间戳回退，
进而通过共享音视频同步时钟累积出约300 ms的视频SR偏移。

补充修复只改G711 RTP输入/重打包路径：解包时保留精确样本时钟和独立NTP映射，
结合原始seq、SSRC和有效负载样本数处理幅度受限的相位波动。真实序号缺口、超出
幅度限制的累计偏移与重启保留明确边界；小幅SR校时只缓慢校正NTP，不重新编号RTP样本。不修改通用Stamp、
FFmpeg音频滤镜或播放器缓存。正常包和短暂波动不新增下一包等待。

首个实机候选使用“两包”上限，但后续发现10 ms偏移会持续3–4包再返回，仍会误判。
最终规则只限制相对原始累计样本轴的偏移幅度，不限制持续包数，也不每包重新锚定。
因此小幅固定偏移和限内且没有序号缺口的真实暂停无法区分，会一并规范化；不宣称
能够保留每一个小于阈值的源端暂停。真实累计漂移超过幅度限制仍会产生明确边界。

具体幅度阈值、异常重启的一包确认及最近100包元数据边界，见
[G711时钟回归fixture说明](fixtures/README-g711-clock.md)。现场749个RTP包、6个SR
的脱敏元数据会走真实RtpTrackImp→G711Decoder→Encoder路径重放；payload为测试生成，
不包含真实音频、地址或凭据。

模拟整链的抖动验证使用：

```bash
python3 tests/test_rtsp_g711_sender_clock.py \
  --server release/linux/Release/MediaServer --fixture fixture.h265 \
  --ffmpeg /absolute/path/to/test-ffmpeg --duration 60 \
  --audio-rate 32000 --audio-codec PCMA --bounce-ms 10 --bounce-hold-packets 5 --sr-jitter-ms 3
```

测试检查G711相邻包的样本区间精确衔接、每个SR间隔的NTP/RTP差值、视频单帧时间
跳变和稳定态告警，不能仅以首尾平均速率或日志行数作为通过条件。该模拟测试仍不
代替实机NVR声音、录像声画同步及延迟验收。

相关的既有 `test_vp9_rtp` 仍失败：`truncated P_DIFF: decoder did not recover
after a parse failure`。已独立编译并绑定58c3e814的原版RtpInfo造包函数重放同一
VP9路径，得到相同失败；相关VP9测试/解码源码均未修改。原因是恢复测试使用的
两字节payload不满足其关键帧识别条件。本次未顺带修改VP9，也不声称全仓测试通过。

## 同 SSRC 重启叠加时间戳波动回归

当接收会话保留、上游使用同一SSRC重置序号时，重启确认不能要求相邻候选的
时间戳精确相差前包样本数。真实RtpTrackImp排序后，0/±10 ms相位交替会使该
精确匹配始终失败：301包已交付，解包器却只输出重启前的1包。

确认条件现允许前后两包各自相位界限之和，同时保留连续序号、历史包拒绝及
最多一个候选包。正常样本轴和NTP策略不变；双包确认阈值不放宽正常单包阈值。

| 2026-09-04 最终本地 Debug 回归 | 结果 |
| --- | --- |
| G711 | 72组通过，含22组新增重启回归 |
| AAC与音频时钟 | 14组通过 |
| 原始重启加抖动复现 | 301包交付、301帧输出，无候选样本丢失 |
| 确认保护 | ±25/40 ms边界接受，超界1 sample拒绝，重复/缺口序号及旧流恢复通过 |

新增矩阵包含16/32 kHz、单/双声道及PCMA/PCMU。上表记录2026-09-04的本地
验证；包含该修复的2026-09-08部署结果见下文。声音与录像同步仍需单独验收。

## G711 批次共用时间戳规范化

2026-09-08 的现场输入为 PCMA/16000 单声道，每包256字节（16 ms），
三个包与两个包交替成批，同一批共用RTP时间戳。组间时间戳推进30/40/50 ms，
因此旧的逐包半包时长阈值会对每个包报出 `-256`、`224`、`384` 或 `544` 样本偏差。

兼容逻辑只作用于G711 RTP解包时钟，不新增API或配置项。连续序号、同SSRC的
共享时间戳提供批次证据后，媒体时间仍按payload样本数连续推进，NTP观察继续扣除
原始RTP相位偏差。每个完整包当次输出，不新增下一包等待。

| 边界 | 行为 |
| --- | --- |
| 批次识别 | 同SSRC、连续序号、原始时间戳相同；重复序号不计为新样本 |
| 批次幅度 | 相对固定累计样本轴最多80 ms，不按每批重新锚定 |
| 同戳总时长 | 批次额外宽容最多80 ms，之后回到普通相位阈值；保留原先有效的变量长包，时间戳长期冻结仍产生明确间断 |
| 恢复普通输入 | 最后一个共享时间戳包起点之后80 ms样本时间到期，恢复原半包/20 ms阈值 |
| 丢包、重启、参数变化、越界 | 保留明确间断并清除批次兼容状态 |
| 批内缺包且时间戳仍相同 | 在已确认批次幅度内立即输出缺口边界，避免误入重启候选而额外丢样本 |
| 从批尾单包加入 | 在首次共享时间戳证据出现前，可能保留一次初始边界 |

启用兼容期间，完整序号覆盖下的限内真实暂停/重叠与批次时间戳存在歧义，
会一并规范化；不宣称保留每个限内源端暂停。普通输入的原始阈值与同SSRC
重启候选确认规则保持不变。详见 [fixture边界说明](fixtures/README-g711-clock.md)。

新增现场fixture只包含940包脱敏元数据，没有真实音频、地址、凭据或SR。
测试使用确定性NTP映射，经真实RtpTrackImp→解包→重打包路径重放每个可能的
加入位置，分别检查立即输出、样本区间、payload守恒和NTP/RTP对应。

合成相机可以用以下命令复现三包/两包共用时间戳，并叠加原始时间戳波动与SR抖动：

```bash
python3 tests/test_rtsp_g711_sender_clock.py \
  --server release/linux/Release/MediaServer --fixture fixture.h265 \
  --duration 20 --audio-rate 16000 --audio-codec PCMA \
  --batch-timestamps --bounce-ms 10 --sr-jitter-ms 3
```

| 2026-09-08 Linux ARM64 Docker 验证 | 结果 |
| --- | --- |
| G711专项 | 89组通过，包含新增7组批次回归 |
| 现场元数据 | 940个加入位置，共442270包；payload守恒、分段样本轴与NTP/RTP对应通过 |
| AAC与音频时钟专项 | 14组通过 |
| PCMA/16000批次输入，20秒，±10 ms相位与3 ms SR抖动 | 音频样本缺口0、回退0，时钟告警0 |
| PCMU/32000普通输入，20秒，±10 ms保持5周期与3 ms SR抖动 | 音频样本缺口0、回退0，时钟告警0 |

### 2026-09-08 RK3588 203 实机验证

11:18:38（北京时间）替换203的 `/opt/zlmediakit/MediaServer` 并重启
`zlmediakit.service`。配置未改，原先在线的12路RTSP流全部恢复。
旧二进制、配置和unit备份在 `/opt/zlmediakit/backups/g711-batch-20260908-1115/`。
本次为手工替换二进制，未修改Debian包版本；实际部署文件的SHA-256为
`42b5752475e4082f1a265d60eb8080bb6ced362dc5bb0670be9d9e1f4fb35ee7`。

| 实机检查 | 结果 |
| --- | --- |
| 新进程启动至11:35:30，约17分钟 | `G711 RTP discontinuity` 为0；仍保留3次明确的NTP时钟边界 |
| 源端60秒被动抓包 | `sub/4` 仍按三包/两包共用时间戳，3750个音频包；输入未被改成正常逐包时钟 |
| 新版 `sub/4` 两轮各60秒 | 每轮统计2901个输出音频包，样本区间不连续0、回退0；58秒RTP推进与收包时间一致 |
| 新版 `sub/4` 第二轮 | 音视频RTP与SR全部通过；音频相邻SR的NTP/RTP差不超过4 ms |
| 同机备份旧版，隔离端口拉同源约65秒 | 4064次G711 RTP告警；音频测量3626包中2175次非递增、3625次样本区间不连续 |
| `live/1`、`live/2`、`live/4` 各25秒 | 音视频RTP/SR与AAC AU检查通过 |

以上样本区间不连续指RTP时间戳与payload样本数不匹配，不等同于网络丢包。
隔离旧版实例在对照结束后已停止，203保留新版运行。

尚未通过的检查也保留在验收结论中：首轮 `sub/4` 视频出现一次−303 ms步进，
对应SR区间偏差343 ms；同时存在约−302 ms的G711 NTP时钟边界。
`live/3` 两轮均有视频步进异常，第二轮另发现一个完整AAC AU回退52 ms。
随后180秒同步采集源端、FFmpeg读入/发布端与ZLM输出，确认 `live/3` 发布RTP
连续，但SR映射硬重锚使输出视频出现−14/−59/−37 ms步进，AAC出现−166 ms
回退及一次不同完整AU同戳。按payload长度/CRC配对并重放NtpStamp计算，2778个
AAC包与1136个可直接配对的视频包均精确匹配输出RTP。该路径未包含在11:18部署的G711补丁中；后续SR修复见下节。
`sub/4` 之前约300 ms的跳变未在同窗复现，仍缺该历史事件的源SR证据。
G711规范化回归通过不代表这些问题已解决。
本次未验收真实NVR声音、录像声画同步或长期时钟漂移。

## SR 校时与 AAC 样本时间戳

本节为历史部署记录，描述2026-09-08 12:34版本，不能作为当前最小候选的行为说明。
该版本稳态捕获通过，后续兼容性审查发现重启、换源、VOD及频差/停供风险；相关扩展已
从当前候选撤出，原SR问题继续作为独立失败门槛。以下保留当时的实现与测试证据。

当时的后续修复覆盖已确认的 FFmpeg SR 相位变化和 ZLM 硬重锚问题。
先前的 G711 成批同戳规范化继续保留；API 请求、配置项和 SDP 不变。

| 路径 | 修复后行为 | 兼容边界 |
| --- | --- | --- |
| FFmpeg `+rtcp_from_packet` | 首个 RTSP 媒体包建立音视频共同的 NTP/PTS 起点；后续 SR 用该固定关系计算当前 NTP 对应的 RTP 时间戳 | 仍为 opt-in；未开启时保留原行为；不重写媒体 PTS |
| ZLM `NtpStamp` | 首个有效 SR 校准起点；后续 SR 偏差按媒体正向推进最多 1 ms/s 消化 | 晚到首个 SR 仍可触发一次启动校准；不消除源端绝对时钟错误 |
| RTP 接收 | 排序后才推进时钟；排序前提供无副作用预览；SR 绑定到接收 SSRC | clear/已接受的新 SSRC 重建时钟；重复包和 B 帧回退不增加校时预算 |
| 时间戳边界 | 整数余数保留分数采样时间与 32 位回环；真实正向停顿保留时长 | 至少 3 秒的大幅反向变化仍按重启边界处理，等待新 SR |
| AAC RTP → Track → RTP | 保留原始样本时间戳及 AU-index 偏移，分片沿用首片时钟；ADTS 头的添加/拆分传递元数据 | 普通 Frame、显式 FrameStamp 或采样率改变仍按 DTS 生成 RTP；不掩盖真实源端样本间断 |
| RTP 首包为 0 | 正常建立输出 NTP 和首个 SR | 0 是合法 RTP 时间戳 |

验证命令（Linux ARM64 构建环境）：

```bash
cmake -S . -B .build/linux-arm64/zlmediakit -DENABLE_TESTS=ON
cmake --build .build/linux-arm64/zlmediakit --target \
  test_rtp_ntp_clock test_aac_rtp_clock test_g711_rtp test_rtsp_audio_clock MediaServer -j6
release/linux/Release/test_rtp_ntp_clock
release/linux/Release/test_aac_rtp_clock
release/linux/Release/test_g711_rtp
release/linux/Release/test_rtsp_audio_clock
```

`test_rtp_ntp_clock` 使用不含媒体内容的同步 RTP/SR 元数据回放，同时经过
receiver、解码器、真实 AAC/G711/H.265 Track、muxer 和 SR 生成路径。
覆盖 SR 回退/同戳、同戳分片、采样精度、正向停顿、重复包、SSRC 和启动边界。
`test_aac_rtp_clock` 覆盖 16/44.1 kHz、原始 RTP 回环/间断、AU 聚合/分片、
ADTS 兼容、首包 0 和显式时间戳覆盖。

### 2026-09-08 12:34 RK3588 203 部署

已于北京时间12:34:14将 SR/AAC 修复与上述 G711 规范化一起部署到
`192.168.1.203`，重启 `zlmediakit`。所有原先在线的12路RTSP流均恢复，
四个实际转码进程的 `/proc/<pid>/exe` 校验值与新版 FFmpeg 一致。

| 项目 | 实机值 |
| --- | --- |
| ZLM | `/opt/zlmediakit/MediaServer`，SHA-256 `14d79941de3ca6b9c821f5360695aacdf82b9236625daa5f3bd6014e4617366e` |
| FFmpeg | `/opt/media-compress/compress`，版本 `1033a46-dirty.2bfd7fb`，SHA-256 `803b688fbe0a25673a814d77359d479b38327ed1281f2ad6d8f498d030f97a64` |
| 回滚备份 | `/opt/zlmediakit/backups/sr-clock-20260908-123413`，包含两份原程序、ZLM配置、unit与部署清单 |
| 配置和依赖 | ZLM/Stream Forge配置与6个原有媒体库文件校验值均未变化 |
| ARM64回归 | `test_rtp_ntp_clock`、`test_aac_rtp_clock`、`test_g711_rtp`、`test_rtsp_audio_clock` 全部通过 |
| FFmpeg专项 | FATE RTP/SR真实字节、共同音视频起点、延迟首包与突发发送通过；旧代码负对照失败 |
| live/1～4 各25秒 | 音视频 RTP/SR 与 AAC AU 检查全部通过 |
| sub/4 60秒 | 2901个G711输出包，样本间断0、回退0，音视频SR检查通过 |

首轮同步窗口为12:34:54～12:37:54，AF_PACKET收到519728包，内核丢包0，
所有观察链路RTP序号连续。FFmpeg发布端与ZLM出口的live/3视频均为40 ms；
AAC保持64 ms（含源端正负1 tick量化）。出口6处同戳均为同一AU的合法分片。
35份音频和35份视频的FFmpeg SR显示固定映射，最大相位变化远小于1 tick。
ZLM出口AAC SR仍使用毫秒NTP，最大相位差约0.938 ms，不能称为样本级NTP精度。

该窗口的工具记录了一次到期超时：live/3数据已收到179.986秒，采集于180.117秒
结束。后续将“窗口到期且尾部仍有数据”明确识别为正常收尾；此前原始记录保留。

第二轮独立同步窗口为12:39:51～12:42:51，180.033秒，AF_PACKET收到489577包，
内核丢包0，工具错误0，各观察链路RTP序号连续。

| 第二轮检查 | 结果 |
| --- | --- |
| live/3 FFmpeg发布与ZLM输出 | 视频40 ms；完整AAC AU回退0、同戳0 |
| AAC内容与样本时钟配对 | 2812/2812个发布AU全部在出口找到，包含5个重新分片的AU；raw RTP逐tick一致 |
| sub/4源G711 | 11251包，仍为2/3包批次交替共戳 |
| sub/4输出G711 | 9009包全部320字节，9008个相邻步进全部320 samples |
| sub/4视频 | 4501帧与源对齐，所有相邻步进误差≤1 ms；原有30/50 ms源间隔保留，累计校时未超过1 ms/s预算 |
| sub/4视频SR | 源端区间相位仍为−102～+37 ms；出口NTP/RTP区间一致 |

两轮都保留了加入播放时的缓存前缀，配对统计只对同时观察到的发布/源数据作结论。
部署后至最终复核，未出现G711连续性或RTP/NTP时间戳异常告警；原有离线源仍重试。
首个有效SR的一次性校准和相机SR的绝对时间偏置仍属于上述兼容边界。
本次验收流恢复、包结构、样本时间戳及SR一致性，未进行人工听音或NVR录像声画验收。

## 不包含的能力

- 不承诺所有音频编码和所有封装互通。RTMP/HTTP-FLV不会因为开启协议转换而自动转码。
  非8k G711不能伪装成8k来获得兼容；当前产品live音频已转AAC，可沿用此路径。
- AAC的960-sample/HE-AAC聚合包仍需ASC/constantDuration专项支持，本轮不声称覆盖。
- RawEncoder主动推送时若显式指定不匹配的PT0/8，会拒绝该轨道并记录日志，
  不擅自重写协商PT；本轮没有扩展上层HTTP启动接口的错误传播。
- 隔离测试不等于实机验收。发布前须保留原二进制/配置，验证H.265海康实时播放、
  声音、录像声画同步、VLC播放计时，以及所有通道重连恢复；异常时回滚。
- Debug/裁剪功能的本地测试构建不能直接替代生产发行包。

协议依据：[RFC 3551（PT与音频RTP规则）](https://www.rfc-editor.org/rfc/rfc3551)
和 [RFC 3640（MPEG-4 AU header与分片）](https://www.rfc-editor.org/rfc/rfc3640)。
