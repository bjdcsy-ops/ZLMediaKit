# 四摄像头 IPC 编码实机测试（2026-09-08）

## 结果

ZLM 与 FFmpeg ARM64 产物已重新构建，在 RK3588 `192.168.1.203` 的隔离目录运行。
共完成 **46 组编码组合，加 1 组 90 秒分层验证**。G711/AAC 主矩阵 **28/28** 均有
摄像头输入、ZLM 转发、FFmpeg 转码后 RTSP 输出三段音视频；但 **28/28** 在 proxy
或 relay 出现完整单元时间戳非递增，**候选不满足生产替换条件**。

本轮没有继续扩大 ZLM/FFmpeg 运行时代码改动。测试结果不是“所有格式兼容通过”，
也不能以没有 G711 告警代替时钟、解码与流恢复验证。

## 构建产物与差异

| 项目 | 产物/版本 | SHA-256 |
| --- | --- | --- |
| ZLM tar | `artifact/zlmediakit-debian11-arm64.tar.gz` | `e41dcff4f30f2e8dd3e43728dc1a2c1d64b14b6e2763c142653fa51b9d4589d8` |
| ZLM MediaServer | HEAD `07253110` 加最小候选 | `f7f2678bd55bf936093d1e747d341ea203724fbe881089df6a3aaa3d75434b98` |
| FFmpeg tar | `artifact/ffmpeg-rockchip-rk3588-1033a46-dirty.23734da-20260908.tar.gz` | `c8f84bbca4def03223334a1aab0aa3795a768201b47bfda5994f4a6f3b67c7f8` |
| FFmpeg 二进制 | `1033a46-dirty.23734da`，包内 MPP/RGA | `dcfbccc6c21f6d6d9c79e1593bd56453d28fd0f2ad7b7769921fcc43ac4c5759` |

FFmpeg tar 的完整校验值以同目录 `SHA256SUMS-ipc-test.txt` 为准。包内 manifest、依赖装载、
目标机二进制校验已执行。版本中的构建日期采用源码提交的 SOURCE_DATE_EPOCH，不代表现场测试日期。

- ZLM 最小候选相对当前 HEAD：3 个运行时文件，+30/−3；相对官方基线：21 个运行时文件，
  +737/−158，其中音频及必要 RTP 辅助部分 12 文件 +525/−75，其余为已有 HTTP 修复。
- FFmpeg 运行时只保留 `rtpenc_aac.c` 的零延迟当前 AU 立即发送条件；原 `rtcp_from_packet`
  语义保留，没有本轮固定 origin 方案。
- 详细上游口径见 [上游差异核对](/Users/chexn/Code/Github/ZLMediaKit/tests/REVIEW-upstream-audio-delta.md)。

203 保留目录为 `/tmp/ipc-codec-matrix-20260908`，候选分别在
`zlmediakit-debian11-arm64/` 和 `ffmpeg-rockchip-rk3588-ubuntu22-arm64/`。
生产 `/opt/zlmediakit/MediaServer`、`/opt/media-compress/compress` 和 ZLM 配置未替换。

## 覆盖范围

| 摄像头 | 视频 | G711/AAC 主矩阵 | 额外组合 | 合计 |
| --- | --- | ---: | ---: | ---: |
| 大华圆头 .108 | 主流 H264/H265，子流 MJPEG | 6 | 9 | 15 |
| 海康圆头 .120 | 主流 H264/H265 | 4 | 0 | 4 |
| 海康长头 .121 | 主流 H264/H265，子流 MJPEG | 6 | 5 | 11 |
| 海康双头 .164 | 101、201 两个镜头各 H264/H265 | 12 | 4 | 16 |

主矩阵把 H264/H265 与设备声明的 G711A、G711U、AAC 交叉测试；.120 只声明 G711A/AAC，
没有强行启用 G711U。双头的两个视频输入分别测试；扩展音频共享同一个物理音频输入，
扩展格式在 101 测一次。不是全部分辨率、帧率、采样率或码率的全排列。

主矩阵大华 G711 为 16 kHz、AAC 为原 64 kHz；额外覆盖 G711A/U 8 kHz、AAC 16 kHz。
海康主矩阵 G711 为 8 kHz、AAC 为 16 kHz。采样率来自实际 SDP/RTP，不能仅靠配置页面判定。

## 测试路径与判据

```text
相机 → 候选 ZLM source → 候选 FFmpeg → 候选 ZLM relay → RTSP 读者
         输入/输出采集       发布输入采集          输出采集
```

ZLM 使用独立端口，`directProxy=0`、`modify_stamp=0`、`lowLatency=1`。
H264/H265 使用 RKMPP 解码及同格式 RKMPP 编码；输出 AAC 16 kHz / 48 kbps，沿用现场
`aresample=16000,asetpts=STARTPTS+N/SR/TB`，`muxdelay=0`、`rtcp_from_packet`。
MJPEG 两组使用视频 copy 路径，不能把它们视为硬件转码成功。

每格观测 35–60 秒、排除 3 秒启动段；另有 .164 H265/AAC 的 90 秒验证。
采集 SDP、RTP/RTCP 元数据、完整音频 AU 与视频 NAL 的大小/hash、解码日志；没有录制摄像头画面或声音。
部分批次在两个独立实例并行执行，不能把各格的到达时间差直接当成相同负载下的延迟基准。

媒体存在、解码成功、样本时钟连续、SR 映射稳定是不同判据。AAC 的合法分片同戳、
±1 tick 或毫秒 Frame 量化不等于完整 AU 回退；扩展编码的“可解码”同样不代表时钟通过。

## 主要问题

### SR 重映射造成真实 AAC 回退

90 秒验证中，同一对完整 AAC AU 在 FFmpeg publisher 为 **+64 ms**，经过 ZLM 后为
**−3069 ms**；.120 H265/G711A 对照为 **+64 ms → −213 ms**。
两组 2016 个完整 AU 的大小/hash 全部同序匹配；25/25 个 publisher SR 的精确 NTP
换算值与对应 AU 的出口 RTP timestamp 相等，误差 0 tick。

证据定位到 ZLM 接收 SR 后的时间重映射与重新封包路径。FFmpeg 的原始 AAC RTP 在这些
完整捕获中保持正向。不能把问题笼统写成“FFmpeg 编码时间戳回退”，也不能把已有公共
NtpStamp 行为说成本轮 batch 补丁新引入。

详见 [完整 AU/SR 分层验证](/Users/chexn/Code/Github/ZLMediaKit/.build/field-tests/20260908/layer-clock-verification.md)。

### 大华的两种输入时钟行为

- G711：原始 40 ms 包存在约 30 ms 相位变化，当前普通 bounce 上限是 20 ms；
  拆成 20 ms 包后可出现 −10 ms。H264/PCMA 的源 +30 ms 次数与出口 −10 ms 次数均为 17。
  这一边界已在 HEAD 中；该输入没有同戳包，不会触发本轮新增 batch allowance。
- AAC 64 kHz：2626 个完整 AU 中的相邻间隔有 1575 次为 0，确为不同内容 AU 成批共用时间戳，
  不是分片计数错误。当前 AAC 路径保留了多数源共戳。

详见 [首批输入行为复核](/Users/chexn/Code/Github/ZLMediaKit/.build/field-tests/20260908/analysis-primary.md)。

### 扩展编码结果

| 分类 | 组数 | 具体组合 |
| --- | ---: | --- |
| 可解码，音频解码日志 0 错误 | 6 | 大华 PCM、G711A/U 8k、AAC16k；海康长头/双头 MP2L2 |
| 有输出但音频解码报错 | 1 | 大华 MP2L2：514 成功帧、514 解码错误 |
| ZLM proxy 缺音轨 | 8 | 大华/长头/双头各 G726、G722.1；长头/双头 PCM |
| FFmpeg 识别/decoder 不可用 | 1 | 大华 G729：代理保留 PT18，FFmpeg 识别为 Audio:none |
| MJPEG 探测/尺寸初始化失败 | 2 | 大华、长头子流，relay 未建立 |

这些 18 组均完成设备设置和实际格式核对，没有把早期 HTTP 500 当作编码不支持。
G726/G722.1 的代理无音轨发生在 FFmpeg 之前；G722.1 与 G722 也不能当作同一种编码。
完整逐项说明见 [编码覆盖分析](/Users/chexn/Code/Github/ZLMediaKit/.build/field-tests/20260908/codec-coverage-analysis.md)。

## 设置与恢复

1. 先保存不可覆盖原始快照；海康同时保存 StreamingChannel 和全局 TwoWayAudio 配置。
2. 海康按原厂网页使用独立音频接口，AAC 同时设置采样率/码率；G711 不携带 AAC 专用字段。
   .121 切换时出现短暂离线，增加稳定等待后完成采集；最终恢复原 G711。
3. 大华主流音频同步所有已有 MainFormat 项，避免配置写 16k、实际仍出 64k。
4. 双头在 H264/H265 切换时会限幅码率，恢复时分步再次写回，最终逐项匹配。
5. .120 会话资源限制使用准确核对的无读者 sub/2 TCP 借槽，保留原 PlayerProxy；
   没有删除代理或猜测重建参数。RTSP Session 标识读取被自动审批拒绝，该读取未执行；
   实际使用的是不读取网络数据的 pidfd/socket 操作。
6. 最后一次切换后，生产 private-input/2 曾保留旧 H264 SDP，而相机已经恢复 H265。
   精确重连原相机输入后重新 DESCRIBE，live/2 恢复 H265；单重启 FFmpeg 无法修复陈旧上游声明。
7. sub/2 等待恢复期间，相机的 6 个会话名额被其他预览占满。.150 退出后，.200/.201
   仍各占 2 个名额；用户暂停这两台设备上的预览后，原 sub/2 自动恢复为 playing，
   .203 恢复两条相机输入连接。相机编码器重置不能解除其他客户端占用。

终检确认大华全部 253 项 Encode 字段、海康共 8 个视频通道及 3 份全局音频配置均与原始
快照一致；生产程序和配置校验值未变，测试媒体进程和临时实例配置均已清理。原有 12 条
RTSP 流全部在线，另有现场新增的 sub/3，因此终检在线总数为 13。
sub/2 为 H265 640×360 / AAC 16 kHz，6 秒复查新增 151 个视频帧和 94 个音频帧，
持续收包正常；目标机本次测试的临时相机凭据文件已删除。

最终相机与生产恢复结果以 [终检记录](/Users/chexn/Code/Github/ZLMediaKit/.build/field-tests/20260908/final-audit.json) 为准。
测试脚本及数据保存在忽略的 `.build/field-tests/20260908/`，未将账号密码写入报告或该证据目录。

## 验证限制与后续门槛

- 早期 publisher 被动探针误用了转发 SDP/PT；相应旧 AU/hash/结构结论作废。只对已保存的 raw
  元数据做独立重标记，未补造缺失轨道。上述强结论来自正确 ANNOUNCE SDP 的完整四层采集。
- .120 H264/G711A 补测源被动捕获有 resync/unmapped，源时钟归因未验证；实际三段音视频存在。
- 部分相机窗口未收到 SR，不能宣称验证了相机绝对 NTP 或端到端音画同步。
- 软件到达时间/hash 配对受采集调度和不同会话影响，出现负值；不作为真实端到端延迟结论。
  本次未做 NVR 人工听音、光学/声学延迟、WebRTC/RTMP/HLS 全协议或长时间稳定性验收。
- 当前只交付比较候选。后续仍需最小范围修复 SR 重锚、G711 相位边界与 AAC 源共戳，
  保留本次失败样本后重新验证，才能考虑替换生产。

逐格机器可读结果：[matrix.csv](/Users/chexn/Code/Github/ZLMediaKit/.build/field-tests/20260908/matrix.csv)。原始 47 个 JSON 和 FFmpeg 日志位于
[结果目录](/Users/chexn/Code/Github/ZLMediaKit/.build/field-tests/20260908/results)。
