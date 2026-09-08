# ZLM 上游差异核对与 G711/AAC 收敛记录

## 当前状态

在原三文件基线完成四摄像头矩阵后，新增 G711 长包窄容差、AAC 完整 AU 共戳规范化和
两处 RTP 回环数值修复。当前工作树相对 HEAD `07253110` 为 **8 个运行时文件 +209/−13**，
相对官方基线 `296dcebe` 为 **22 个运行时文件 +915/−167**；其中音频/RTP 辅助为
13 文件 +703/−84，其余为已有 HTTP 修复。FFmpeg 本轮没有新增变更。

没有新增全局时钟策略，配置默认值和 `modify_stamp` 入口保持不变。139 组 ARM64
兼容/专项回归通过，原 SR 回退门槛仍失败。采用 1.203 本机配置（`modify_stamp=0`）的
冷启动证据与最新实机结果见 [本轮报告](REPORT-ipc-coldstart-minimal-fixes-20260908.md)。

下文各处“三文件最小候选”的数值和逐文件表是本轮三项小修前的历史比较，
保留用于审阅收敛过程，不代表当前工作树规模。

## 基线与统计口径

2026-09-08，通过 `git ls-remote upstream refs/heads/master` 核实官方当前 master 为 `296dcebe8f3e81121e2c835cc9503ac5c25e2a1a`，与本地 upstream/master 一致。当前 HEAD 为 `07253110`，完整包含该官方提交；`upstream/master...HEAD` 左右计数为 `0/7`，其中包含一次合并提交。五个子模块指针及 .gitmodules/.gitmodules_github 均与官方一致。

使用 `git diff --numstat` 统计文本增加/删除，包含注释；不是语义复杂度或性能指标。运行时代码口径为 ext-codec/src/server/webrtc/api，其他分类独立列出。工作树总量只包含已跟踪文件；未跟踪测试/fixture 另列，不含无关 www/http-flv/。

## 差异规模

| 比较范围 | 全部已跟踪文件 | 增加/删除 | 运行时代码文件 | 运行时增加/删除 |
| --- | ---: | ---: | ---: | ---: |
| 官方 → 已提交 HEAD | 45 | +6002/−327 | 21 | +709/−157 |
| 官方 → 收敛前工作树 | 51 | +6774/−400 | 27 | +919/−230 |
| HEAD → 收敛前未提交 | 16 | +785/−86 | 12 | +212/−75 |

| 官方 → 收敛前工作树分类 | 文件数 | 增加/删除 |
| --- | ---: | ---: |
| 构建、CI、配置 | 13 | +1044/−170 |
| 运行时代码 | 27 | +919/−230 |
| 文档 | 4 | +582/−0 |
| 测试及 fixture | 7 | +4229/−0 |

收敛前未跟踪的四个测试/fixture 共 3384 行：G711 批次 metadata 944、SR metadata 1595、AAC test 233、NtpStamp test 612；测试已随 API 撤回重新编写，两个原始 fixture 未变。大量总行数来自测试与采样元数据，不能称为几千行核心重构。另一方面，少量全局时钟代码会影响多协议，不能只看行数判定安全。

## 已提交差异的来源

| 独立切片 | 提交 | 处置 |
| --- | --- | --- |
| G711 参数/样本时钟、AAC AU 修复 | e33da079、9dea6b9c | 本次继续核对，作为已有局部音频基线 |
| HTTP API cookie、HTTP 日期与退出响应 | 58c3e814 | 已有独立修复，保留；含 RtspSession 的 HTTP Date 变更 |
| RTSP PLAY RTP-Info 回归 | 172a30d2 | 测试和文档，保留 |
| 单份 Debian 11 ARM64 构建 | 9a420e69、07253110 | 已有独立构建需求，保留 |
| 同步官方更新 | e5f1c8ec | 已包含官方最新，保留 |

已提交音频运行时代码共 12 文件 +497/−74；已有 HTTP 相关运行时代码共 9 文件 +212/−83。音频辅助改动包括参数传入 decoder/encoder、样本级 makeRtpWithStamp 和 RawEncoder 边界，不是新的通用时钟框架。

## 最小候选的可量化范围

保留已提交 HEAD，另取 G711Rtp.cpp/.h 的批次兼容，以及 RtspMuxer 首个 raw RTP=0 的一行初始化修复；其他运行时 dirty 改动恢复 HEAD 内容。这一范围现已应用到实际工作树。

收敛前备份：`/private/tmp/zlm-before-minimal-20260908-134349`，包含 tracked.patch、状态及相关未跟踪文件。原比较快照为 `/private/tmp/zlm-upstream-minimal-review-i9so82st`。

| 比较范围 | 运行时代码文件 | 增加/删除 |
| --- | ---: | ---: |
| 收敛前本轮未提交 | 12 | +212/−75 |
| 最小候选相对已提交 HEAD | 3 | +30/−3 |
| 最小候选相对官方 | 21 | +737/−158 |

三文件候选具体为 G711Rtp.cpp +27/−2、G711Rtp.h +2/−0、RtspMuxer.cpp +1/−1。最后一项可独立审阅；若只保留批次兼容则为两文件 +29/−2。跨基线的 numstat 不能简单相减代替直接 diff。

最小候选的 G711/AAC 及必要 RTP 辅助部分为 12 文件 +525/−75，其余仍是已有 HTTP 修复。相对已提交基线增加很少，但 G711 专用解包与样本时钟本身已显著多于官方，不能将整个 fork 宣称为只有三行小修。

## 逐文件运行时差异

| 文件 | 收敛前对官方 | 当前最小候选对官方 |
| --- | ---: | ---: |
| `ext-codec/AAC.cpp` | +31/−6 | 与官方一致 |
| `ext-codec/AACRtp.cpp` | +109/−38 | +91/−33 |
| `ext-codec/AACRtp.h` | +22/−3 | +7/−1 |
| `ext-codec/G711.cpp` | +33/−14 | +33/−14 |
| `ext-codec/G711.h` | +3/−2 | +3/−2 |
| `ext-codec/G711Rtp.cpp` | +290/−18 | +290/−18 |
| `ext-codec/G711Rtp.h` | +61/−2 | +61/−2 |
| `server/WebApi.cpp` | +18/−26 | +18/−26 |
| `src/Common/Stamp.cpp` | +68/−51 | 与官方一致 |
| `src/Common/Stamp.h` | +9/−1 | 与官方一致 |
| `src/Http/HttpClient.cpp` | +1/−1 | +1/−1 |
| `src/Http/HttpConst.cpp` | +146/−1 | +146/−1 |
| `src/Http/HttpConst.h` | +24/−0 | +24/−0 |
| `src/Http/HttpCookie.cpp` | +15/−40 | +15/−40 |
| `src/Http/HttpCookieManager.cpp` | +3/−5 | +3/−5 |
| `src/Http/HttpCookieManager.h` | +2/−2 | +2/−2 |
| `src/Http/HttpSession.cpp` | +1/−4 | +1/−4 |
| `src/Rtp/RawEncoder.cpp` | +19/−0 | +19/−0 |
| `src/Rtp/RawEncoder.h` | +1/−0 | +1/−0 |
| `src/Rtsp/RtpCodec.cpp` | +6/−2 | +6/−2 |
| `src/Rtsp/RtpCodec.h` | +7/−1 | +7/−1 |
| `src/Rtsp/RtpReceiver.cpp` | +31/−2 | 与官方一致 |
| `src/Rtsp/RtpReceiver.h` | +8/−3 | 与官方一致 |
| `src/Rtsp/RtspDemuxer.cpp` | +1/−0 | +1/−0 |
| `src/Rtsp/RtspMuxer.cpp` | +6/−2 | +6/−2 |
| `src/Rtsp/RtspPlayer.cpp` | +1/−1 | 与官方一致 |
| `src/Rtsp/RtspSession.cpp` | +3/−5 | +2/−4 |

## 撤回边界与明确代价

1. 撤出本轮全局 NtpStamp 平滑、RtpReceiver 排序时钟/Preview/pending SR、新 setter 及配套调用。Stamp、RtpReceiver、RtspPlayer 恢复到官方；RtspSession 仅恢复本轮 SR 调用，保留已有 HTTP Date 修复。
2. 撤出 AACRtpFrame/raw 绝对时间戳透传和 AAC.cpp 中配套 metadata 拷贝。AAC encoder 与 AACTrack 恢复原结构，保留已提交的完整 AU 当前时间、聚合及分片正确性修复。
3. 接受原 Frame 毫秒精度：正常 44.1 kHz/1024 AU 可呈现 23/24 ms，48 kHz 为 21/22 ms，16 kHz 为 64 ms；该量化不等于新增排队或逐帧无界积累，不再承诺输出逐 AU 样本精确。
4. 保留局部 G711 的采样率/声道、按样本重打包、立即解包、批次容差、丢包及重启边界。不能恢复官方硬编码 8 kHz/单声道，否则已确认现场问题会回来。
5. 不实施上轮的 source/steady 参数、AAC 来源代次框架、其他 codec 接入路由、Stream Forge 音频配置或滤镜变更。

## 尚未关闭的 SR 问题

此候选是贴近官方的对照基线，不是可直接部署的完整替代。原 live/3 已证明 FFmpeg 发送 SR 相位变化与 ZLM 官方硬重锚共同造成视频/AAC 回退；撤出全局平滑及 AAC raw 后，这个问题仍然存在。不能把源 RTP 连续误写为整条链路连续，也不能删掉原失败回放来取得通过。

FFmpeg 固定起点改动本身有频差/停供风险，隔离候选已撤回至原 rtcp_from_packet；但将原 SR 语义与最小 ZLM 候选组合会重新暴露上述已知问题。原始 SR 数据回放确认视频 40 ms → −59 ms、AAC 约64 ms → −166 ms，以及完整 AAC AU 同戳。因此继续保留独立失败门槛。推迟到排序后、只禁止回退或缓存同戳包都不能同时修复这些反例并保留官方校时/重启语义；本批未再增加公共时钟策略。

AAC 回到普通 Frame/DTS 后，保留 muxer 换源、VOD、ADTS、FrameStamp 和 16/44.1/48 kHz 量化的九组回归通过。该结果只关闭 raw metadata 引入的回归，不表示原版所有时钟行为都没有缺陷。
