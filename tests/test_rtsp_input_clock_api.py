#!/usr/bin/env python3
"""Check input-clock playback controls against an isolated MediaServer.

Usage: python3 tests/test_rtsp_input_clock_api.py --server /path/to/MediaServer
"""

import argparse
import socketserver
import threading
from pathlib import Path
from unittest.mock import patch

import test_http_api_auth as http_test


class RtspHandler(socketserver.StreamRequestHandler):
    def handle(self):
        while True:
            line = self.rfile.readline()
            if not line:
                return
            method, url, _ = line.decode().strip().split(" ", 2)
            headers = {}
            while True:
                line = self.rfile.readline()
                if line in (b"\r\n", b"\n", b""):
                    break
                key, value = line.decode().split(":", 1)
                headers[key.lower()] = value.strip()
            self.rfile.read(int(headers.get("content-length", "0")))
            body = ""
            extra = "Session: input-clock-fixture\r\n"
            if method == "OPTIONS":
                extra += "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN\r\n"
            elif method == "DESCRIBE":
                replay_range = "a=range:npt=0-60\r\n" if url.endswith("/replay") else ""
                body = ("v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=controls\r\nt=0 0\r\n"
                        + replay_range + "a=control:*\r\nm=audio 0 RTP/AVP 0\r\n"
                        "a=rtpmap:0 PCMU/8000/1\r\na=control:trackID=0\r\n")
                extra += f"Content-Type: application/sdp\r\nContent-Base: {url}/\r\n"
            elif method == "SETUP":
                extra += "Transport: RTP/AVP/TCP;unicast;interleaved=0-1;ssrc=12345678\r\n"
            self.wfile.write((f"RTSP/1.0 200 OK\r\nCSeq: {headers['cseq']}\r\n"
                              f"{extra}Content-Length: {len(body)}\r\n\r\n{body}").encode())
            with self.server.changed:
                self.server.requests.append((method, headers))
                self.server.changed.notify_all()
            if method == "TEARDOWN":
                return


class RtspFixture(socketserver.ThreadingTCPServer):
    daemon_threads = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), RtspHandler)
        self.requests = []
        self.changed = threading.Condition()

    def count(self):
        with self.changed:
            return len(self.requests)

    def next_request(self, previous):
        with self.changed:
            if not self.changed.wait_for(lambda: len(self.requests) > previous, timeout=3):
                raise http_test.TestFailure("API succeeded without sending the expected RTSP request")
            return self.requests[previous]


def run(base_url, secret, timeout):
    def api(name, params, parameter_mode="json"):
        return http_test.request(base_url, "/index/api/" + name, {"secret": secret, **params},
                                 timeout=timeout, parameter_mode=parameter_mode)[1]

    # A requested-but-ineligible replay must retain the same control path as
    # mode 0. Checking requested alone would incorrectly reject this case.
    for label, requested, effective, replay in (
        ("default", None, 0, False),
        ("off", 0, 0, False),
        ("on", 1, 1, False),
        ("replay", 1, 0, True),
    ):
        with RtspFixture() as source:
            worker = threading.Thread(target=source.serve_forever, daemon=True)
            worker.start()
            key = None
            try:
                stream = {"vhost": "__defaultVhost__", "app": "clock-control-test", "stream": label}
                params = {**stream, "url": f"rtsp://127.0.0.1:{source.server_address[1]}/"
                          + ("replay" if replay else "live"), "rtp_type": 0, "retry_count": 0,
                          "enable_rtsp": 0, "enable_rtmp": 0, "enable_hls": 0,
                          "enable_hls_fmp4": 0, "enable_mp4": 0, "enable_ts": 0, "enable_fmp4": 0}
                if requested is not None:
                    params["rtsp_input_clock"] = requested
                result = api("addStreamProxy", params, "form")
                http_test.assert_code(result, 0, f"add {label}")
                key = result["data"]["key"]
                result = api("getProxyInfo", {"key": key})
                http_test.assert_code(result, 0, f"inspect {label}")
                info = result["data"]
                if (info["inputClockRequested"], info["inputClockMode"], info["status"]) != (
                        requested or 0, effective, 0):
                    raise http_test.TestFailure(f"unexpected proxy mode/status for {label}")

                if source.next_request(3)[0] != "PLAY":
                    raise http_test.TestFailure("fixture did not finish the RTSP handshake")
                before = source.count()
                controls = (("pauseStream", {}, "PAUSE", None),
                            ("seekStream", {"position": 2}, "PLAY", "range"),
                            ("setStreamSpeed", {"speed": 2}, "PLAY", "scale"))
                for name, args, method, header in controls:
                    result = api(name, {**stream, **args})
                    http_test.assert_code(result, -300 if effective else 0, f"{label} {name}")
                    if effective:
                        if "rtsp_input_clock" not in result.get("msg", "") or result.get("result") == 0:
                            raise http_test.TestFailure(f"{name} did not explicitly reject unsupported control")
                    else:
                        if result.get("result") != 0 or result.get("msg") != "success":
                            raise http_test.TestFailure(f"legacy success response changed for {name}")
                        actual_method, headers = source.next_request(before)
                        if actual_method != method or (header and header not in headers):
                            raise http_test.TestFailure(f"unexpected upstream control for {label} {name}")
                        before += 1

                # The allowed PLAY Scale:1 is also a TCP ordering barrier: any
                # incorrectly forwarded rejected control would precede it.
                result = api("setStreamSpeed", {**stream, "speed": 1})
                http_test.assert_code(result, 0, f"{label} normal speed")
                if result.get("result") != 0 or result.get("msg") != "success":
                    raise http_test.TestFailure("normal-speed success response changed")
                method, headers = source.next_request(before)
                if method != "PLAY" or headers.get("scale") != "1":
                    raise http_test.TestFailure("unsupported controls reached the RTSP source")
            finally:
                try:
                    if key is not None:
                        http_test.assert_code(api("delStreamProxy", {"key": key}), 0, f"delete {label}")
                finally:
                    source.shutdown()
                    worker.join(timeout=3)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--config", type=Path)
    parser.add_argument("--certificate", type=Path)
    args = parser.parse_args()
    # Reuse the existing isolated config, loopback listener, random secret,
    # readiness check and guaranteed process cleanup; only API checks differ.
    with patch.object(http_test, "run", run):
        http_test.run_managed_server(args.server, args.config, args.certificate, 5, 15)
    print("test_rtsp_input_clock_api passed (4 modes, 16 controls)")


if __name__ == "__main__":
    main()
