#!/usr/bin/env python3
"""Check that RTSP PLAY RTP-Info describes the actual first RTP packets."""

import argparse
import base64
from collections import deque
import posixpath
import re
import socket
import struct
import sys
import time
from urllib.parse import unquote, urljoin, urlsplit, urlunsplit


class TestFailure(RuntimeError):
    pass


def without_userinfo(url):
    parsed = urlsplit(url)
    host = parsed.hostname or ""
    if ":" in host:
        host = f"[{host}]"
    if parsed.port is not None:
        host = f"{host}:{parsed.port}"
    return urlunsplit((parsed.scheme, host, parsed.path, parsed.query, parsed.fragment))


def canonical_url(url):
    parsed = urlsplit(without_userinfo(url))
    scheme = parsed.scheme.lower()
    host = (parsed.hostname or "").lower()
    if ":" in host:
        host = f"[{host}]"
    port = parsed.port
    if port is not None and not (
        (scheme == "rtsp" and port == 554) or (scheme == "rtsps" and port == 322)
    ):
        host = f"{host}:{port}"
    path = posixpath.normpath(parsed.path or "/")
    if parsed.path.endswith("/") and path != "/":
        path += "/"
    return urlunsplit((scheme, host, path, parsed.query, ""))


def resolve_control_url(base_url, control):
    if urlsplit(control).scheme:
        return without_userinfo(control)
    base = without_userinfo(base_url)
    return urljoin(base if base.endswith("/") else base + "/", control)


def signed32(value):
    value &= 0xFFFFFFFF
    return value if value < 0x80000000 else value - 0x100000000


def parse_sdp(body, base_url):
    tracks = []
    current = None
    for raw_line in body.decode("utf-8", errors="replace").splitlines():
        line = raw_line.strip()
        if line.startswith("m="):
            fields = line[2:].split()
            if len(fields) < 4:
                continue
            payload_types = []
            for field in fields[3:]:
                try:
                    payload_types.append(int(field))
                except ValueError:
                    pass
            current = {
                "media": fields[0],
                "payload_types": payload_types,
                "clocks": {},
                "control": None,
            }
            tracks.append(current)
        elif current is not None and line.lower().startswith("a=rtpmap:"):
            match = re.match(r"a=rtpmap:(\d+)\s+[^/]+/(\d+)", line, re.IGNORECASE)
            if match:
                current["clocks"][int(match.group(1))] = int(match.group(2))
        elif current is not None and line.lower().startswith("a=control:"):
            control = line.split(":", 1)[1].strip()
            if control != "*":
                current["control"] = resolve_control_url(base_url, control)
    return [track for track in tracks if track["control"]]


def parse_rtp_info(value, base_url):
    entries = {}
    for raw_entry in value.split(","):
        fields = {}
        for raw_field in raw_entry.strip().split(";"):
            if "=" not in raw_field:
                continue
            key, field_value = raw_field.split("=", 1)
            fields[key.strip().lower()] = field_value.strip()
        try:
            control = canonical_url(resolve_control_url(base_url, fields["url"]))
            entries[control] = {
                "seq": int(fields["seq"]),
                "rtptime": int(fields["rtptime"]),
            }
        except (KeyError, ValueError) as ex:
            raise TestFailure(f"invalid RTP-Info entry: {raw_entry.strip()!r}") from ex
    return entries


class RtspClient:
    def __init__(self, url, timeout):
        parsed = urlsplit(url)
        if parsed.scheme.lower() != "rtsp" or not parsed.hostname:
            raise TestFailure("the URL must use rtsp:// and include a host")
        self.url = without_userinfo(url)
        self.timeout = timeout
        self.sock = socket.create_connection(
            (parsed.hostname, parsed.port or 554), timeout=timeout
        )
        self.sock.settimeout(timeout)
        self.buffer = b""
        self.frames = deque()
        self.cseq = 0
        self.session = None
        self.authorization = None
        if parsed.username is not None:
            username = unquote(parsed.username)
            password = unquote(parsed.password or "")
            token = base64.b64encode(f"{username}:{password}".encode()).decode()
            self.authorization = f"Basic {token}"

    def close(self):
        self.sock.close()

    def _receive(self):
        data = self.sock.recv(65536)
        if not data:
            raise TestFailure("RTSP server closed the connection")
        self.buffer += data

    def _pop_interleaved(self):
        if not self.buffer or self.buffer[0] != 0x24:
            return None
        while len(self.buffer) < 4:
            self._receive()
        length = struct.unpack("!H", self.buffer[2:4])[0]
        while len(self.buffer) < 4 + length:
            self._receive()
        channel = self.buffer[1]
        payload = self.buffer[4 : 4 + length]
        self.buffer = self.buffer[4 + length :]
        return channel, payload

    def _read_response(self):
        while True:
            frame = self._pop_interleaved()
            if frame is not None:
                self.frames.append(frame)
                continue
            marker = self.buffer.find(b"\r\n\r\n")
            if marker < 0:
                self._receive()
                continue
            if not self.buffer.startswith(b"RTSP/"):
                raise TestFailure(
                    f"unexpected data before RTSP response: {self.buffer[:32]!r}"
                )
            head = self.buffer[:marker]
            lines = head.decode("utf-8", errors="replace").split("\r\n")
            try:
                status = int(lines[0].split()[1])
            except (IndexError, ValueError) as ex:
                raise TestFailure(f"invalid RTSP status line: {lines[0]!r}") from ex
            headers = {}
            for line in lines[1:]:
                if ":" not in line:
                    continue
                key, value = line.split(":", 1)
                key = key.strip().lower()
                value = value.strip()
                headers[key] = f"{headers[key]},{value}" if key in headers else value
            try:
                content_length = int(headers.get("content-length", "0"))
            except ValueError as ex:
                raise TestFailure("invalid RTSP Content-Length") from ex
            body_start = marker + 4
            while len(self.buffer) < body_start + content_length:
                self._receive()
            body = self.buffer[body_start : body_start + content_length]
            self.buffer = self.buffer[body_start + content_length :]
            return status, headers, body

    def request(self, method, url=None, headers=None, allow_failure=False):
        self.cseq += 1
        request_url = without_userinfo(url or self.url)
        lines = [
            f"{method} {request_url} RTSP/1.0",
            f"CSeq: {self.cseq}",
            "User-Agent: ZLMediaKit-RTP-Info-regression-test",
        ]
        if self.session:
            lines.append(f"Session: {self.session}")
        if self.authorization:
            lines.append(f"Authorization: {self.authorization}")
        for key, value in (headers or {}).items():
            lines.append(f"{key}: {value}")
        self.sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode())
        status, response_headers, body = self._read_response()
        session = response_headers.get("session")
        if session:
            self.session = session.split(";", 1)[0]
        if status != 200 and not allow_failure:
            raise TestFailure(f"{method} returned RTSP status {status}")
        return status, response_headers, body

    def next_frame(self, deadline):
        if self.frames:
            return self.frames.popleft()
        while time.monotonic() < deadline:
            frame = self._pop_interleaved()
            if frame is not None:
                return frame
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            self.sock.settimeout(remaining)
            try:
                self._receive()
            except TimeoutError:
                break
        raise TestFailure("timed out waiting for first RTP packets")


def run_session(url, timeout, expected_audio_clock):
    client = RtspClient(url, timeout)
    try:
        client.request("OPTIONS")
        _, describe_headers, sdp = client.request(
            "DESCRIBE", headers={"Accept": "application/sdp"}
        )
        base_url = describe_headers.get(
            "content-base", describe_headers.get("content-location", client.url)
        )
        tracks = parse_sdp(sdp, base_url)
        if not tracks:
            raise TestFailure("DESCRIBE returned no playable SDP tracks")
        if expected_audio_clock is not None:
            audio_clocks = {
                clock
                for track in tracks
                if track["media"] == "audio"
                for clock in track["clocks"].values()
            }
            if expected_audio_clock not in audio_clocks:
                raise TestFailure(
                    f"expected audio clock {expected_audio_clock}, got "
                    f"{sorted(audio_clocks) or 'no audio track'}"
                )

        channel_to_track = {}
        for index, track in enumerate(tracks):
            requested_rtp_channel = index * 2
            _, setup_headers, _ = client.request(
                "SETUP",
                track["control"],
                {
                    "Transport": (
                        "RTP/AVP/TCP;unicast;"
                        f"interleaved={requested_rtp_channel}-{requested_rtp_channel + 1}"
                    )
                },
            )
            match = re.search(
                r"(?:^|;)\s*interleaved\s*=\s*(\d+)\s*-\s*(\d+)",
                setup_headers.get("transport", ""),
                re.IGNORECASE,
            )
            if not match:
                raise TestFailure("SETUP response did not negotiate interleaved channels")
            rtp_channel = int(match.group(1))
            if rtp_channel in channel_to_track:
                raise TestFailure(f"duplicate RTP channel {rtp_channel}")
            track["rtp_channel"] = rtp_channel
            channel_to_track[rtp_channel] = track

        _, play_headers, _ = client.request(
            "PLAY", headers={"Range": "npt=0.000-"}
        )
        if "rtp-info" not in play_headers:
            raise TestFailure("PLAY response did not include RTP-Info")
        advertised = parse_rtp_info(play_headers["rtp-info"], base_url)

        first_packets = {}
        deadline = time.monotonic() + timeout
        while len(first_packets) < len(tracks):
            channel, payload = client.next_frame(deadline)
            track = channel_to_track.get(channel)
            if track is None or canonical_url(track["control"]) in first_packets:
                continue
            if len(payload) < 12 or payload[0] >> 6 != 2:
                continue
            control = canonical_url(track["control"])
            first_packets[control] = {
                "payload_type": payload[1] & 0x7F,
                "seq": struct.unpack("!H", payload[2:4])[0],
                "rtptime": struct.unpack("!I", payload[4:8])[0],
                "ssrc": struct.unpack("!I", payload[8:12])[0],
            }

        summaries = []
        for track in tracks:
            control = canonical_url(track["control"])
            entry = advertised.get(control)
            packet = first_packets[control]
            if entry is None:
                raise TestFailure(f"RTP-Info has no entry for {control}")
            clock = track["clocks"].get(packet["payload_type"])
            seq_ok = entry["seq"] == packet["seq"]
            stamp_ok = entry["rtptime"] == packet["rtptime"]
            if not seq_ok or not stamp_ok:
                tick_delta = signed32(packet["rtptime"] - entry["rtptime"])
                time_delta = (
                    f" ({tick_delta * 1000 / clock:.3f} ms)" if clock else ""
                )
                raise TestFailure(
                    f"{track['media']} {control}: advertised "
                    f"seq={entry['seq']} rtptime={entry['rtptime']}, first RTP "
                    f"seq={packet['seq']} rtptime={packet['rtptime']}; "
                    f"timestamp delta={tick_delta} ticks{time_delta}"
                )
            summaries.append(
                f"{track['media']}:seq={packet['seq']},rtptime={packet['rtptime']}"
            )
        return "; ".join(summaries)
    finally:
        try:
            if client.session:
                client.request("TEARDOWN", allow_failure=True)
        except (OSError, TestFailure):
            pass
        client.close()


def parse_args():
    parser = argparse.ArgumentParser(
        description=(
            "Open RTSP-over-TCP sessions and require each PLAY RTP-Info entry "
            "to match the actual first RTP packet for that track."
        )
    )
    parser.add_argument("url", help="RTSP URL; userinfo is never printed")
    parser.add_argument(
        "--sessions", type=int, default=1, help="number of fresh sessions (default: 1)"
    )
    parser.add_argument(
        "--timeout", type=float, default=10.0, help="per-session timeout in seconds"
    )
    parser.add_argument(
        "--expect-audio-clock",
        type=int,
        help="require an SDP audio clock rate, for example 44100",
    )
    args = parser.parse_args()
    if args.sessions < 1:
        parser.error("--sessions must be at least 1")
    if args.timeout <= 0:
        parser.error("--timeout must be greater than 0")
    return args


def main():
    args = parse_args()
    safe_url = without_userinfo(args.url)
    for index in range(1, args.sessions + 1):
        try:
            summary = run_session(
                args.url, args.timeout, args.expect_audio_clock
            )
        except (OSError, TestFailure) as ex:
            print(
                f"FAIL session {index}/{args.sessions} {safe_url}: {ex}",
                file=sys.stderr,
            )
            return 1
        print(f"PASS session {index}/{args.sessions} {summary}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
