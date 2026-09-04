#!/usr/bin/env python3
"""Synthetic PCMU/16000 + HEVC proxy test. No camera or external service is used.

Generate a one-second HEVC fixture with AUDs and repeated parameter sets:
  ffmpeg -f lavfi -i testsrc2=size=128x96:rate=25 -t 1 -c:v libx265 \
    -x265-params aud=1:repeat-headers=1:keyint=25:bframes=0 -f hevc fixture.h265
Run this test with a MediaServer executable and the fixture. The server is
started without hooks on ephemeral loopback ports and is stopped on exit.
"""

import argparse
import base64
import json
import re
import socket
import struct
import subprocess
import tempfile
import threading
import time
import urllib.parse
import urllib.request
from pathlib import Path

from test_rtsp_play_rtp_info import RtspClient, parse_sdp, signed32, TestFailure


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def hevc_units(path):
    nals = [n for n in re.split(b"\x00\x00\x00?\x01", Path(path).read_bytes()) if n]
    config = {n[0] >> 1 & 63: n for n in nals if n[0] >> 1 & 63 in (32, 33, 34)}
    units, unit = [], []
    for nal in nals:
        if nal[0] >> 1 & 63 == 35 and unit:
            units.append(unit)
            unit = []
        unit.append(nal)
    if unit:
        units.append(unit)
    if len(units) != 25 or len(config) != 3:
        raise TestFailure("fixture must contain 25 HEVC AUs with AUD/VPS/SPS/PPS")
    return units, config


def timestamp_bounce(index, amplitude, hold_packets):
    # The CLI caps each hold at eight packets: [8,16) and [17,25) cannot
    # overlap or cross the next one-second cycle. One packet preserves the
    # original fixture's exact timestamp pattern.
    phase = index % 25
    return (-amplitude if 8 <= phase < 8 + hold_packets else
            amplitude if 17 <= phase < 17 + hold_packets else 0)


class Camera(threading.Thread):
    def __init__(self, fixture, audio_rate=16000, audio_codec="PCMU", bounce_ms=0, sr_jitter_ms=0,
                 bounce_hold_packets=1):
        super().__init__(daemon=True)
        self.units, config = hevc_units(fixture)
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(1)
        self.sock.settimeout(1)
        self.port = self.sock.getsockname()[1]
        self.stop_event = threading.Event()
        self.error = None
        self.client = None
        self.audio_rate = audio_rate
        self.audio_pt = 0 if audio_codec == "PCMU" else 8
        self.bounce_ticks = round(bounce_ms * audio_rate / 1000)
        self.bounce_hold_packets = bounce_hold_packets
        self.sr_jitter_ms = sr_jitter_ms
        params = ";".join(f"sprop-{name}={base64.b64encode(config[k]).decode()}"
                          for k, name in ((32, "vps"), (33, "sps"), (34, "pps")))
        self.sdp = ("v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=Clock fixture\r\n"
                    "t=0 0\r\na=control:*\r\n"
                    "m=video 0 RTP/AVP 98\r\na=rtpmap:98 H265/90000\r\n"
                    f"a=fmtp:98 {params}\r\na=control:trackID=0\r\n"
                    f"m=audio 0 RTP/AVP {self.audio_pt}\r\n"
                    f"a=rtpmap:{self.audio_pt} {audio_codec}/{audio_rate}/1\r\n"
                    "a=control:trackID=1\r\n").encode()

    def close(self):
        self.stop_event.set()
        if self.client:
            self.client.close()
        self.sock.close()
        self.join(timeout=2)

    def run(self):
        try:
            while not self.stop_event.is_set():
                try:
                    self.client, _ = self.sock.accept()
                    break
                except socket.timeout:
                    continue
            if not self.client:
                return
            self.client.settimeout(8)
            buf, channels = b"", {}
            while True:
                while b"\r\n\r\n" not in buf:
                    data = self.client.recv(65536)
                    if not data:
                        return
                    buf += data
                head, buf = buf.split(b"\r\n\r\n", 1)
                lines = head.decode().split("\r\n")
                method, url, _ = lines[0].split()
                headers = dict(line.split(":", 1) for line in lines[1:] if ":" in line)
                headers = {k.lower(): v.strip() for k, v in headers.items()}
                extra, body = "", b""
                if method == "DESCRIBE":
                    body = self.sdp
                    extra = f"Content-Type: application/sdp\r\nContent-Base: {url}/\r\n"
                elif method == "SETUP":
                    track = int(url.rsplit("=", 1)[1])
                    match = re.search(r"interleaved=(\d+)-(\d+)", headers["transport"])
                    if not match:
                        raise TestFailure("fixture requires RTSP interleaved TCP")
                    channels[track] = int(match.group(1))
                    extra = f"Transport: {headers['transport']}\r\n"
                reply = (f"RTSP/1.0 200 OK\r\nCSeq: {headers['cseq']}\r\n"
                         f"Session: clock-fixture\r\n{extra}Content-Length: {len(body)}\r\n\r\n")
                self.client.sendall(reply.encode() + body)
                if method == "PLAY":
                    break
            self.send_media(channels)
        except (OSError, ValueError, KeyError, TestFailure) as ex:
            if not self.stop_event.is_set():
                self.error = str(ex)

    def send_media(self, channels):
        started, epoch = time.monotonic(), time.time()
        seq, counts, octets = [100, 200], [0, 0], [0, 0]
        ssrc, base = [0x11223344, 0x55667788], [0x10000000, 0x20000000]

        def send(track, body, rtcp=False):
            self.client.sendall(struct.pack("!BBH", 36, channels[track] + int(rtcp), len(body)) + body)

        def rtp(track, payload, ticks, marker):
            header = struct.pack("!BBHII", 0x80, (98 if track == 0 else self.audio_pt) | (int(marker) << 7),
                                 seq[track] & 65535, ticks & 0xFFFFFFFF, ssrc[track])
            send(track, header + payload)
            seq[track] += 1
            counts[track] += 1
            octets[track] += len(payload)

        index = 0
        while not self.stop_event.is_set():
            if self.stop_event.wait(max(0, started + index * .04 - time.monotonic())):
                break
            samples = self.audio_rate * 40 // 1000
            # Continuous 40 ms payloads with held negative/positive source
            # phase offsets. Never delay or duplicate a source packet.
            bounce = timestamp_bounce(index, self.bounce_ticks, self.bounce_hold_packets)
            stamps = [base[0] + index * 3600, base[1] + index * samples + bounce]
            for pos, nal in enumerate(self.units[index % 25]):
                last = pos == len(self.units[index % 25]) - 1
                if len(nal) <= 1200:
                    rtp(0, nal, stamps[0], last)
                else:
                    body = nal[2:]
                    for off in range(0, len(body), 1197):
                        end = off + 1197 >= len(body)
                        fu = bytes([(nal[0] & 0x81) | (49 << 1), nal[1],
                                    (nal[0] >> 1 & 63) | (0x80 if off == 0 else 0) | (0x40 if end else 0)])
                        rtp(0, fu + body[off:off + 1197], stamps[0], last and end)
            rtp(1, bytes((index * samples + i) & 255 for i in range(samples)), stamps[1], True)
            if index % (25 if self.sr_jitter_ms else 125) == 0:
                ntp = epoch + index * .04 + 2208988800
                for track in (0, 1):
                    report_ntp = ntp
                    if track == 1 and index:
                        report_ntp += self.sr_jitter_ms / 1000 * (1 if index // 25 % 2 else -1)
                    send(track, struct.pack("!BBHIIIIII", 0x80, 200, 6, ssrc[track], int(report_ntp),
                                            int((report_ntp % 1) * 2**32), stamps[track] & 0xFFFFFFFF,
                                            counts[track], octets[track]), True)
            index += 1


class AacAuClock:
    """Validate this fixture's AAC-hbr single-AU RTP output at AU boundaries."""

    def __init__(self):
        self.pending = None
        self.previous = None
        self.last_complete = None
        self.completed = 0
        self.fragments = 0
        self.nonincreasing = 0
        self.contexts = []
        self.errors = []

    def observe(self, body):
        seq, stamp, ssrc = struct.unpack_from("!HII", body, 2)
        packet = dict(seq=seq, timestamp=stamp, ssrc=ssrc, pt=body[1] & 127, marker=bool(body[1] & 128))
        continuation = False
        error = None
        try:
            offset = 12 + 4 * (body[0] & 15)
            if body[0] >> 6 != 2 or offset > len(body):
                raise ValueError("invalid RTP header")
            if body[0] & 16:
                if offset + 4 > len(body):
                    raise ValueError("truncated RTP extension")
                offset += 4 + 4 * struct.unpack_from("!H", body, offset + 2)[0]
            end = len(body)
            if body[0] & 32:
                if not body[-1] or body[-1] > end - offset:
                    raise ValueError("invalid RTP padding")
                end -= body[-1]
            payload = body[offset:end]
            if len(payload) < 5 or struct.unpack_from("!H", payload)[0] != 16:
                raise ValueError("expected one nonempty AAC-hbr AU header")
            header = struct.unpack_from("!H", payload, 2)[0]
            packet.update(au_size=header >> 3, au_index=header & 7,
                          payload_size=len(payload), fragment_size=len(payload) - 4)
            if not packet["au_size"] or packet["au_index"]:
                raise ValueError("invalid single-AU size/index")
            received = packet["fragment_size"]
            if self.pending:
                for key in ("ssrc", "pt", "timestamp", "au_size", "au_index"):
                    if packet[key] != self.pending[key]:
                        raise ValueError("AAC fragment changed " + key)
                if seq != (self.pending["seq"] + 1) & 65535:
                    raise ValueError("AAC fragment sequence is missing, duplicate or reordered")
                received += self.pending["received"]
                continuation = True
            elif self.last_complete:
                if ssrc != self.last_complete["ssrc"] or signed32(stamp - self.last_complete["timestamp"]) <= 0:
                    raise ValueError("completed AAC AU timestamp is not increasing")
            if received > packet["au_size"] or packet["marker"] != (received == packet["au_size"]):
                raise ValueError("AAC fragment length and final marker disagree")
            if continuation:
                self.fragments += 1
            if packet["marker"]:
                self.completed += 1
                self.last_complete = packet
                self.pending = None
            else:
                self.pending = dict(packet, received=received)
        except (ValueError, struct.error) as exception:
            error = str(exception)
            continuation = False
            self.pending = None
            if len(self.errors) < 8:
                self.errors.append(dict(error=error, previous=self.previous, current=packet))
        if self.previous and signed32(stamp - self.previous["timestamp"]) <= 0:
            self.nonincreasing += 1
            if len(self.contexts) < 8:
                self.contexts.append(dict(previous=self.previous, current=packet,
                                          valid_same_au_fragment=continuation, error=error))
        self.previous = packet
        return continuation


def test_aac_au_clock():
    def packet(seq, stamp, size, fragment, marker, ssrc=17):
        return (struct.pack("!BBHIIHH", 0x80, 96 | (128 if marker else 0), seq, stamp, ssrc, 16, size << 3)
                + bytes(fragment))
    first = packet(65535, 10000, 700, 584, False)
    last = packet(0, 10000, 700, 116, True)
    cases = [
        ("valid fragments and sequence wrap", [first, last, packet(1, 11024, 200, 200, True)], True),
        ("duplicate complete AU", [packet(1, 10000, 200, 200, True), packet(2, 10000, 200, 200, True)], False),
        ("duplicate fragment", [first, first], False),
        ("missing fragment bytes", [first, packet(0, 10000, 700, 100, True)], False),
        ("fragment sequence gap", [first, packet(1, 10000, 700, 116, True)], False),
        ("fragment SSRC change", [first, packet(0, 10000, 700, 116, True, 18)], False),
        ("backwards AU", [packet(1, 10000, 200, 200, True), packet(2, 9000, 200, 200, True)], False),
        ("missing final fragment", [first], False),
    ]
    for name, packets, valid in cases:
        clock = AacAuClock()
        for body in packets:
            clock.observe(body)
        if (not clock.errors and clock.pending is None) != valid:
            raise TestFailure("AAC AU checker self-test failed: " + name)
        if valid and (clock.fragments != 1 or clock.nonincreasing != 1 or clock.completed != 2):
            raise TestFailure("AAC AU checker lost raw packet evidence: " + name)


def check_stream(url, duration, g711=True, audio_rate=16000):
    client = RtspClient(url, 5)
    try:
        _, headers, sdp = client.request("DESCRIBE", headers={"Accept": "application/sdp"})
        tracks = parse_sdp(sdp, headers.get("content-base", url))
        stats, channel_map = {}, {}
        for i, track in enumerate(tracks):
            pt = track["payload_types"][0]
            clock = track["clocks"].get(pt, 8000 if pt in (0, 8) else 90000)
            client.request("SETUP", track["control"], {"Transport": f"RTP/AVP/TCP;unicast;interleaved={2*i}-{2*i+1}"})
            channel_map[2*i] = track["media"]
            stats[track["media"]] = dict(clock=clock, pt=pt, packets=0, bytes=0, reverse=0,
                                          sample_gaps=0, last_payload=0, large_steps=[], first=None, last=None, sr=[])
        client.request("PLAY")
        started = time.monotonic()
        aac_clock = None if g711 else AacAuClock()
        while time.monotonic() < started + duration or (aac_clock and aac_clock.pending):
            channel, body = client.next_frame(started + duration + .1)
            media = channel_map.get(channel & ~1)
            if not media or len(body) < 12:
                continue
            row = stats[media]
            if channel & 1:
                if body[1] == 200 and len(body) >= 28:
                    sec, frac, ticks = struct.unpack_from("!III", body, 8)
                    if time.monotonic() - started > 2:
                        row["sr"].append([time.monotonic() - started, sec + frac / 2**32, ticks])
            else:
                # Build AU state during warmup too; never start measurement in
                # the middle of a fragment group without its preceding state.
                continuation = aac_clock.observe(body) if media == "audio" and aac_clock else False
                if time.monotonic() - started <= 2:
                    continue
                ticks = struct.unpack_from("!I", body, 4)[0]
                point = [time.monotonic() - started, ticks]
                if row["last"] and media == "audio" and signed32(ticks - row["last"][1]) <= 0:
                    if g711 or signed32(ticks - row["last"][1]) < 0 or not continuation:
                        row["reverse"] += 1
                if row["last"] and media == "audio" and g711:
                    if signed32(ticks - row["last"][1]) != row["last_payload"]:
                        row["sample_gaps"] += 1
                if row["last"] and media == "video":
                    step = signed32(ticks - row["last"][1]) / row["clock"]
                    if step < 0 or step > .08:
                        row["large_steps"].append(step)
                row["first"] = row["first"] or point
                row["last"] = point
                row["packets"] += 1
                row["bytes"] += len(body) - 12
                row["last_payload"] = len(body) - 12
        failures = []
        audio = stats.get("audio", {})
        if aac_clock:
            audio["aac_au_check"] = dict(completed_aus=aac_clock.completed, valid_fragment_continuations=aac_clock.fragments,
                                         raw_packet_nonincreasing=aac_clock.nonincreasing,
                                         nonincreasing_contexts=aac_clock.contexts, errors=aac_clock.errors)
            if aac_clock.errors or aac_clock.pending:
                failures.append("AAC access units failed fragment or timestamp validation")
        expected_clock = audio_rate if g711 else 16000
        if audio.get("clock") != expected_clock or (expected_clock != 8000 and audio.get("pt", 0) < 96):
            failures.append("audio must retain its sample clock and use an appropriate PT")
        if audio.get("reverse", 0):
            failures.append("audio has overlapping or backwards RTP sample intervals")
        if audio.get("sample_gaps", 0):
            failures.append("G711 RTP start does not equal the previous packet's sample end")
        for media, row in stats.items():
            if row["large_steps"]:
                failures.append(f"{media}: RTP has a backwards or greater-than-80-ms frame step")
            if not row["first"] or len(row["sr"]) < 2:
                failures.append(f"{media}: insufficient RTP/SR evidence")
                continue
            row["rtp_seconds"] = signed32(row["last"][1] - row["first"][1]) / row["clock"]
            row["wall_seconds"] = row["last"][0] - row["first"][0]
            if not .9 < row["rtp_seconds"] / row["wall_seconds"] < 1.1:
                failures.append(f"{media}: RTP media time does not advance at wall-clock speed")
            first, last = row["sr"][0], row["sr"][-1]
            row["sr_ntp_seconds"] = last[1] - first[1]
            row["sr_rtp_seconds"] = signed32(last[2] - first[2]) / row["clock"]
            if abs(row["sr_ntp_seconds"] - row["sr_rtp_seconds"]) > .1:
                failures.append(f"{media}: sender report NTP and RTP clocks diverge")
            row["sr_interval_errors"] = [
                (b[1] - a[1]) - signed32(b[2] - a[2]) / row["clock"]
                for a, b in zip(row["sr"], row["sr"][1:])
            ]
            if any(abs(error) > .01 for error in row["sr_interval_errors"]):
                failures.append(f"{media}: individual SR interval differs from RTP by over 10 ms")
        if g711 and audio.get("first") and audio.get("last"):
            audio["samples_per_wall_second"] = audio["bytes"] / (audio["last"][0] - audio["first"][0])
            if not .93 * audio_rate < audio["samples_per_wall_second"] < 1.1 * audio_rate:
                failures.append("G711 sample count does not match its declared clock")
        return dict(sdp=sdp.decode(), tracks=stats, failures=failures)
    finally:
        client.close()


def main():
    test_aac_au_clock()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True)
    parser.add_argument("--fixture", required=True)
    parser.add_argument("--ffmpeg", help="Optional FFmpeg with rtcp_from_packet for the AAC live relay")
    parser.add_argument("--duration", type=float, default=14)
    parser.add_argument("--audio-rate", type=int, choices=[8000, 16000, 32000, 48000, 64000], default=16000)
    parser.add_argument("--audio-codec", choices=["PCMA", "PCMU"], default="PCMU")
    parser.add_argument("--bounce-ms", type=float, default=0, help="Transient audio RTP timestamp offset, not a sample gap")
    parser.add_argument("--bounce-hold-packets", type=int, choices=range(1, 9), default=1,
                        help="Consecutive packets per negative/positive phase; default 1 preserves the original fixture")
    parser.add_argument("--sr-jitter-ms", type=float, default=0, help="Alternating audio-only SR/NTP anchor offset each second")
    args = parser.parse_args()
    if args.duration < 12 or not 0 <= args.bounce_ms < 40 or not 0 <= args.sr_jitter_ms <= 10:
        parser.error("duration must be >=12 seconds; bounce in [0,40) ms; SR jitter in [0,10] ms")
    camera = Camera(args.fixture, args.audio_rate, args.audio_codec, args.bounce_ms, args.sr_jitter_ms,
                    args.bounce_hold_packets)
    camera.start()
    process = relay = None
    with tempfile.TemporaryDirectory(prefix="zlm-g711-clock-") as temporary:
        root = Path(temporary)
        http, rtsp = free_port(), free_port()
        while http == rtsp:
            rtsp = free_port()
        config = root / "config.ini"
        config.write_text(f"[http]\nport={http}\nsslport=0\n[rtsp]\nport={rtsp}\nsslport=0\ndirectProxy=0\nlowLatency=1\n"
                          "[rtmp]\nport=0\nsslport=0\n[shell]\nport=0\n[api]\nsecret=local-regression\n"
                          "[hook]\nenable=0\n[protocol]\nmodify_stamp=0\nenable_audio=1\nadd_mute_audio=0\n"
                          "enable_rtsp=1\nenable_rtmp=0\nenable_hls=0\nenable_mp4=0\nenable_ts=0\nenable_fmp4=0\n"
                          "[rtp]\naudioMtuSize=600\n[general]\nmergeWriteMS=0\n")
        with (root / "server.log").open("w") as log:
            try:
                process = subprocess.Popen([str(Path(args.server).resolve()), "-c", str(config)], cwd=root, stdout=log, stderr=log)
                def api(name, values=None):
                    query = urllib.parse.urlencode({"secret": "local-regression", **(values or {})})
                    with urllib.request.urlopen(f"http://127.0.0.1:{http}/index/api/{name}?{query}", timeout=8) as response:
                        return json.load(response)
                for _ in range(100):
                    try:
                        api("getServerConfig")
                        break
                    except OSError:
                        if process.poll() is not None:
                            raise TestFailure("MediaServer exited during startup")
                        time.sleep(.05)
                result = api("addStreamProxy", dict(vhost="__defaultVhost__", app="private-input", stream="4",
                             url=f"rtsp://127.0.0.1:{camera.port}/fixture", rtp_type=0,
                             enable_rtsp=1, enable_rtmp=0, enable_hls=0, enable_mp4=0))
                if result.get("code") != 0:
                    raise TestFailure(f"addStreamProxy failed: {result}")
                time.sleep(1)
                # The first upstream SR can replace the initial arrival-time
                # anchor. Retain this bootstrap evidence separately; the
                # steady-state assertion must detect recurring clock faults.
                startup_log = (root / "server.log").read_text(errors="replace").splitlines()
                input_url = f"rtsp://127.0.0.1:{rtsp}/private-input/4"
                report = check_stream(input_url, args.duration, audio_rate=args.audio_rate)
                report["source_case"] = dict(codec=args.audio_codec, rate=args.audio_rate,
                                             bounce_ms=args.bounce_ms, sr_jitter_ms=args.sr_jitter_ms,
                                             bounce_hold_packets=args.bounce_hold_packets)
                if args.ffmpeg:
                    output_url = f"rtsp://127.0.0.1:{rtsp}/live/4"
                    with (root / "ffmpeg.log").open("w") as relay_log:
                        relay = subprocess.Popen([
                            str(Path(args.ffmpeg).resolve()), "-nostats", "-loglevel", "error",
                            "-fflags", "nobuffer", "-reorder_queue_size", "0", "-rtsp_transport", "tcp",
                            "-correct_ts_overflow", "0", "-i", input_url,
                            "-map", "0:v:0", "-map", "0:a:0", "-c:v", "copy",
                            "-af", "aresample=16000,asetpts=STARTPTS+N/SR/TB",
                            "-c:a", "aac", "-aac_coder", "fast", "-b:a", "48k", "-ar:a", "16000",
                            "-max_interleave_delta", "1", "-flush_packets", "1", "-muxdelay", "0",
                            "-muxpreload", "0", "-rtpflags", "+rtcp_from_packet", "-f", "rtsp",
                            "-rtsp_transport", "tcp", output_url,
                        ], stdout=relay_log, stderr=relay_log)
                        for _ in range(150):
                            if relay.poll() is not None:
                                raise TestFailure("FFmpeg relay exited: " + (root / "ffmpeg.log").read_text())
                            if any(entry.get("app") == "live" for entry in api("getMediaList").get("data", [])):
                                break
                            time.sleep(.1)
                        else:
                            raise TestFailure("FFmpeg did not publish live/4 within 15 seconds")
                        report["live"] = check_stream(output_url, args.duration, g711=False)
                        report["failures"].extend("live: " + failure for failure in report["live"]["failures"])
                media = api("getMediaList").get("data", [])
                report["metadata"] = [entry.get("tracks") for entry in media if entry.get("app") == "private-input"]
                report["startup_timing_warnings"] = [
                    line for line in startup_log
                    if "rtp stamp abnormal" in line or ("G711" in line and "discontinu" in line.lower())
                ]
                report["timing_warnings"] = [
                    line for line in (root / "server.log").read_text(errors="replace").splitlines()[len(startup_log):]
                    if "rtp stamp abnormal" in line or ("G711" in line and "discontinu" in line.lower())
                ]
                if report["timing_warnings"]:
                    report["failures"].append("server reported abnormal RTP/G711 timing")
                print(json.dumps(report, indent=2))
                if camera.error:
                    raise TestFailure(camera.error)
                if report["failures"]:
                    raise TestFailure("; ".join(report["failures"]))
            finally:
                if relay:
                    relay.terminate()
                    try:
                        relay.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        relay.kill()
                        relay.wait()
                camera.close()
                if process:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()


if __name__ == "__main__":
    main()
