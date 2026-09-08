# G711 RTP clock regression fixture

`g711_32k_sr_metadata.txt` contains packet metadata from a 30-second passive
capture of a 32 kHz mono PCMA camera input. It contains no audio samples,
credentials, host addresses, or RTSP URLs. The original RTP sequence numbers,
timestamps, payload lengths and SR/RTP event order are retained. NTP is rebased
to 1000000 ms at the first captured sender report.

| Column | Meaning |
|---|---|
| kind | `S`: RTCP sender-report anchor; `R`: audio RTP packet |
| relative_wall_us | Capture receipt time relative to the first SR; no sleeps are required |
| sequence | Original 16-bit RTP sequence; zero for SR rows |
| raw_rtp_timestamp | Original RTP sample timestamp, including camera jitter |
| payload_bytes | G711 bytes; zero for SR rows |
| normalized_ntp_ms | Rebased SR NTP; zero for RTP rows |

The test synthesizes deterministic G711 payload bytes, replays anchors through
`RtpTrackImp::setNtpStamp`, and sends RTP through the real receiver, decoder and
encoder. SR rows are inserted before their recorded next RTP packet, preserving
ordering when multiple events had the same capture wall timestamp.

`g711_16k_batch_metadata.txt` contains 940 packets from a 15-second passive
capture of mono PCMA/16000 with 256-byte payloads. Groups of three and two packets
alternate, sharing a timestamp within each group. Group timestamps advance by
30, 40 or 50 ms although every packet contains 16 ms of samples. Its four columns
are relative receipt microseconds, sequence, raw RTP timestamp and payload bytes.
It contains no payload, addresses, credentials or captured sender reports. The
receiver replay uses a fixed synthetic NTP mapping and synthetic payloads, and
checks every possible starting packet, including joins at the tail of a batch.

`g711_16k_long_packet_phase_metadata.txt` contains four fixed-40 ms PCMA/PCMU
windows (4202 packets total). Columns and window headers are defined in the file.
It preserves sequence, raw timestamp, payload length and relative receipt time,
but intentionally excludes captured SRs to isolate the raw long-packet phase.
Every suffix is replayed through `RtpTrackImp` and the real decoder/encoder with
synthetic payloads and a synthetic initial NTP anchor: 2,209,202 input packets and
4,418,404 output packets, with sample/content conservation.

## Correction boundaries

These checks cover the G711 RTP decoder/encoder path. They do not change the
clock policy of other codecs or override an explicit `FrameStamp` timestamp.

| Situation | Expected behavior |
|---|---|
| Complete, in-order normal or corrected RTP packet | Emit its decoded Frame immediately; no next-packet wait |
| Bounded raw RTP phase offset, with continuous sequence and complete samples | Normalize within `min(20 ms, current packet duration / 2)`, relative to the original cumulative sample axis; the number of offset packets does not reset this axis or create a discontinuity |
| Same SSRC, seq+1, positive raw advance and equal adjacent packet lengths longer than 20 ms | Additionally normalize absolute cumulative phase strictly below `min(previous packet duration, 40 ms)`; this allowance never widens restart probation or short/variable packet rules |
| Small fixed offset or a return from a long bounded excursion | Keep continuous samples; the raw phase must remain inside the same absolute bound |
| Continuous sequence and same SSRC with repeated raw timestamp | Normalize the batch by payload samples; the extra allowance bounds the same-timestamp run and absolute phase error to 80 ms, falling back to the ordinary phase bound beyond that run duration |
| After the last shared timestamp | Retain the batch allowance for 80 ms of sample time from that packet's start, then apply the ordinary or eligible long-packet bound; charge variable payload duration after checking each packet's start |
| Larger cumulative phase error, sequence gap, or changed SSRC | Preserve an explicit discontinuity and clear batch allowance; sequence gaps remain boundaries even if their raw phase error is small |
| Sequence gap inside a known batch | A backwards phase within the active 80 ms bound is an immediate gap boundary, not restart probation; a larger backwards clock still requires restart confirmation |
| Duplicate or known historical packet | Discard without emitting duplicate samples; compare exact sequence/timestamp/payload-size metadata from the latest 100 accepted packets |
| Unrecognized same-SSRC sequence restart | Hold at most one abnormal probation packet; the next consecutive sequence confirms the restart if its timestamp error relative to the first packet's end is within the sum of both packets' phase bounds, then release both in order |
| Positive sequence gap with a backwards RTP clock beyond the phase bound | Use the same restart probation and NTP reanchor; a reset such as `60000 -> 1000` must not add the old/new raw RTP clock difference to the output sample axis |
| Lone abnormal late packet followed by normal input | Discard the probation packet and resume normal immediate output |
| SR/NTP observation error within 1 ms | Keep the existing clock anchor |
| Other small SR/NTP error | Slew the NTP clock by at most 1 ms per second; preserve exact RTP sample progression |
| SR/NTP error larger than 250 ms for 2 seconds in one direction | Apply an explicit clock reanchor; do not suppress a persistent correction indefinitely |
| Cache or timestamp wrapper | Preserve exact metadata through `getCacheAbleFrame`; explicit wrappers use the smoothed PTS fallback, respecting timestamp overrides |

Only the abnormal restart path retains a media packet. The 100-entry history
contains metadata, not audio payload. RTP output still follows its configured
packet duration and MTU; immediate decoder output does not mean zero encoder
packetization delay. Explicit discontinuities may change the output timeline.

Restart confirmation uses the sum of the two phase bounds because the new
sample axis is not yet anchored: the observed pair error is the difference
between their individual phase offsets. Equal or slightly decreasing raw
timestamps can therefore still confirm a restart when sequence numbers are
consecutive. The normal per-packet correction bound is unchanged after release;
a larger offset relative to the first packet's anchor remains an explicit
discontinuity rather than silently dropping the restarted audio.

The signed 16-bit sequence difference alone does not identify a restart.
For a positive sequence gap, a backwards sample-clock error beyond the phase
bound also enters probation. Normal sequence/RTP wrap and forward-time packet
loss retain their raw RTP duration; bounded phase errors keep the existing gap
boundary behavior. A lone candidate is discarded when normal old-stream input
resumes, and duplicate or nonconsecutive candidates cannot confirm a restart.

A complete sequence with a small permanent phase offset is indistinguishable
from a long bounded timestamp excursion. A pause within the same amplitude
limit, without missing sequence numbers, has the same ambiguity. This policy
normalizes those cases; it does not claim to preserve every sub-limit source
pause. It does not normalize unbounded drift: the expected raw timestamp advances
only by accepted payload samples, so accumulated error eventually crosses the
amplitude limit and remains an explicit boundary. Variable payload sizes use the
current packet duration for the original bound; the extra long-packet allowance requires equal adjacent lengths.

The first field candidate used a two-packet excursion limit. The 203 source later
held a 10 ms phase offset for three or four 40 ms packets and returned, causing
false boundaries. This is why the limit is on phase magnitude rather than a
guessed number of consecutive packets.

Batch compatibility also measures phase against the original cumulative sample
axis; it never reanchors at each group or renews a drifting origin. The 16 kHz
capture spans -32 to +20 ms of raw phase, and an arbitrary join point may shift
that origin. The 80 ms limit covers that observed 52 ms span while bounding
compatibility in sample time rather than packet count. A frozen timestamp must
exhaust the same-timestamp run allowance even if repeated packets keep renewing
it. Existing ordinary bounded phase remains valid: a 10 ms packet followed by an
80 ms packet sharing its timestamp still has only -10 ms phase and is accepted
by the original 20 ms bound. Audio-parameter changes and explicit RTP or NTP
discontinuities clear the allowance.

Recognizing a batch requires at least one repeated timestamp with a new sequence
number. If a receiver joins on a batch's last packet, its first timestamp jump
can still create one boundary before that evidence exists. No extra packet is
buffered to resolve this ambiguity. Similarly, an isolated repeated timestamp
temporarily enables compatibility: a real pause/overlap within the active 80 ms
phase bound cannot be distinguished from batching and is normalized. Sequence
gaps, restarts and drift beyond the bound remain explicit. Existing same-SSRC
restart probation rules are unchanged; this compatibility does not promise to
retain an unconfirmed restart candidate when its next packet cannot confirm it.

## Regression coverage

| Cases | Checks |
|---|---|
| 8/16/32/48/64 kHz, mono and stereo | Positive/negative 10 ms bounce, exact samples, MTU and interleaved-sample alignment |
| 32 kHz, 1280-byte input, 600-byte MTU | At most 588 payload bytes per output packet, byte conservation, continuous sample timestamps |
| Captured input | 749 RTP packets and 6 SR anchors through `RtpTrackImp`, with synthetic payload bytes |
| Long-packet phase | All 4202 joins; 40/80 ms packet caps at single-sample boundaries, positive/negative phase, short/variable packets, seq/SSRC changes, frozen raw clocks and persistent bounded phase |
| Sequence boundaries | Loss, duplicates, late packets, consecutive historical packets with variable payload lengths, SSRC restart, sequence and RTP timestamp wrap |
| Same-SSRC restart with bounded timestamp phase | Real `RtpTrackImp` sequence reset at 16/32 kHz, mono/stereo and PCMA/PCMU; probation release, byte conservation and sustained output; pair-boundary acceptance and out-of-bound rejection |
| Same-SSRC backwards clock across the sequence half-range | Real `RtpTrackImp` with an epoch NTP anchor, `20000/33768/33769/60000 -> 1000`, mono/stereo and PCMA/PCMU; sample/RTP reanchor, bounded phase, forward-loss preservation and probation guards |
| Correction limits | 3/4/5/25/250-packet holds, permanent bounded phase and direct sign changes, cumulative drift beyond the magnitude bound, variable packet thresholds, sequence gaps, large jumps, bounded clock slew and persistent large SR correction |
| Compatibility | Existing SDP/static and dynamic PT, WAVE metadata, generic Frame packetization, cache wrappers and timestamp overrides |

Run from the repository root after the existing CMake configuration:

```sh
cmake --build build-audio --target test_g711_rtp -j4
release/linux/Debug/test_g711_rtp
```

The fixture reproduces isolated positive/negative 10 ms RTP phase excursions
and small SR reanchors. It is not a test for every AAC/G711 device, NVR audio
content, real network loss, or end-to-end latency.

## Synchronized SR reanchor metadata

`rtp_sr_reanchor_metadata.txt` contains four windows from the synchronized
180-second capture on RK3588 203 on 2026-09-08. It retains RTP sequence numbers,
raw timestamps, payload lengths, markers, AAC AU sizes, SR pairs and event order.
It excludes media content, payload hashes, addresses and credentials. Each window
rebases both tracks by one common NTP offset, preserving their relative clocks.

| Window | Events | Regression |
| --- | ---: | --- |
| `publisher_video_sr_step` | 494 | Continuous 40 ms HEVC becomes a negative interval after SR replacement |
| `publisher_aac_sr_step` | 318 | Continuous AAC becomes a -166 ms output interval |
| `publisher_aac_sr_plateau` | 171 | Two distinct complete AAC AUs acquire one output timestamp |
| `source_av_sr_progress` | 602 | Camera video/audio SR phase changes reach the following output frame |

`test_rtp_ntp_clock` first replays every receiver event. Its full decoder/track/
muxer/SR replay seeds both tracks with their first recorded SR, then starts media
at their common available time. This avoids manufacturing a multi-second
single-track startup from independently truncated capture windows. All four
subsequent target SR transitions remain covered. Separate tests cover a late
first SR and receiver lifecycle changes explicitly.


## AAC complete-AU batch metadata

`aac_64k_batch_metadata.txt` contains two 2626-AU windows from the earlier Dahua
H264/AAC and H265/AAC matrix. It retains only sequence, RTP timestamp and AU size.
`test_aac_rtp_clock` replays all 5252 joins through the actual decoder with
synthetic payloads (6,898,502 AUs); separate tests exercise AACTrack, muxer and SR
serialization. Those replays isolate raw complete-AU batching, not captured SR
reanchoring. Full current-config cold-start results, including residual SR errors,
are recorded in [the field report](../REPORT-ipc-coldstart-minimal-fixes-20260908.md).
