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

## Correction boundaries

These checks cover the G711 RTP decoder/encoder path. They do not change the
clock policy of other codecs or override an explicit `FrameStamp` timestamp.

| Situation | Expected behavior |
|---|---|
| Complete, in-order normal or corrected RTP packet | Emit its decoded Frame immediately; no next-packet wait |
| Bounded raw RTP phase offset, with continuous sequence and complete samples | Normalize within `min(20 ms, current packet duration / 2)`, relative to the original cumulative sample axis; the number of offset packets does not reset this axis or create a discontinuity |
| Small fixed offset or a return from a long bounded excursion | Keep continuous samples; the raw phase must remain inside the same absolute bound |
| Larger cumulative phase error, sequence gap, or changed SSRC | Preserve an explicit discontinuity; sequence gaps remain boundaries even if their raw phase error is small |
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
current packet duration when calculating that limit.

The first field candidate used a two-packet excursion limit. The 203 source later
held a 10 ms phase offset for three or four 40 ms packets and returned, causing
false boundaries. This is why the limit is on phase magnitude rather than a
guessed number of consecutive packets.

## Regression coverage

| Cases | Checks |
|---|---|
| 8/16/32/48/64 kHz, mono and stereo | Positive/negative 10 ms bounce, exact samples, MTU and interleaved-sample alignment |
| 32 kHz, 1280-byte input, 600-byte MTU | At most 588 payload bytes per output packet, byte conservation, continuous sample timestamps |
| Captured input | 749 RTP packets and 6 SR anchors through `RtpTrackImp`, with synthetic payload bytes |
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
