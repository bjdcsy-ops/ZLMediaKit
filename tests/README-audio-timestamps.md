# G711 与 AAC 音频时钟回归验证

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

FFmpeg配套补丁独立处理 `max_delay=0` 的AAC RTP包化：当前AU立即发送，
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

新增矩阵包含16/32 kHz、单/双声道及PCMA/PCMU。该重启修复尚未部署到203，
本地回归不替代实机重连、声音与录像同步验收。

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
