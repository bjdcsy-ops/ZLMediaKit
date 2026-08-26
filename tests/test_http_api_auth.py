#!/usr/bin/env python3
"""Exercise secret and browser-cookie authentication against MediaServer."""

import argparse
import configparser
import hashlib
import http.client
import json
import os
import re
import secrets
import socket
import subprocess
import sys
import tempfile
import time
from email.utils import format_datetime, parsedate_to_datetime
from pathlib import Path
from urllib.parse import urlencode, urlsplit


class TestFailure(RuntimeError):
    pass


def parse_canonical_http_date(value, context):
    try:
        parsed = parsedate_to_datetime(value)
        canonical = format_datetime(parsed, usegmt=True)
    except (TypeError, ValueError) as ex:
        raise TestFailure(f"invalid {context}: {value!r}") from ex
    if canonical != value:
        raise TestFailure(f"non-canonical {context}: {value!r}")
    return parsed


def request(base_url, path, params=None, cookie=None, timeout=5.0, parameter_mode="query"):
    parsed = urlsplit(base_url)
    if parsed.scheme not in ("http", "https") or not parsed.hostname:
        raise TestFailure("base URL must use http:// or https:// and include a host")
    if parsed.path not in ("", "/") or parsed.query or parsed.fragment:
        raise TestFailure("base URL must not contain a path, query, or fragment")

    headers = {"Accept": "application/json"}
    if cookie:
        headers["Cookie"] = cookie

    encoded_params = urlencode(params or {})
    request_body = None
    if parameter_mode == "query":
        method = "GET"
        target = path + (f"?{encoded_params}" if encoded_params else "")
    elif parameter_mode == "form":
        method = "POST"
        target = path
        request_body = encoded_params.encode("utf-8")
        headers["Content-Type"] = "application/x-www-form-urlencoded"
    elif parameter_mode == "json":
        method = "POST"
        target = path
        request_body = json.dumps(params or {}).encode("utf-8")
        headers["Content-Type"] = "application/json"
    else:
        raise TestFailure(f"unsupported parameter mode: {parameter_mode}")

    connection_type = http.client.HTTPSConnection if parsed.scheme == "https" else http.client.HTTPConnection
    connection = connection_type(parsed.hostname, parsed.port, timeout=timeout)
    try:
        connection.request(method, target, body=request_body, headers=headers)
        response = connection.getresponse()
        raw_body = response.read()
        response_headers = response.getheaders()
    finally:
        connection.close()

    if response.status != 200:
        raise TestFailure(f"{path} returned HTTP {response.status}")
    try:
        body = json.loads(raw_body.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as ex:
        raise TestFailure(f"{path} returned invalid JSON") from ex
    response_dates = [value for name, value in response_headers if name.lower() == "date"]
    if len(response_dates) != 1:
        raise TestFailure(f"{path} returned {len(response_dates)} Date headers")
    parse_canonical_http_date(response_dates[0], "HTTP Date")
    return response_headers, body


def set_cookie_values(headers):
    return [value for name, value in headers if name.lower() == "set-cookie"]


def extract_cookie(values, name):
    prefix = f"{name}="
    for value in values:
        if value.startswith(prefix):
            return value[len(prefix) :].split(";", 1)[0]
    return None


def assert_canonical_expires(values):
    if not values:
        raise TestFailure("response did not contain Set-Cookie")
    for value in values:
        match = re.search(r"(?:^|;)\s*expires=([^;]+)", value, re.IGNORECASE)
        if not match:
            raise TestFailure("Set-Cookie did not contain expires")
        parse_canonical_http_date(match.group(1), "cookie expires")


def response_date(headers):
    value = next(value for name, value in headers if name.lower() == "date")
    return parse_canonical_http_date(value, "HTTP Date")


def cookie_expiry(values, name):
    prefix = f"{name}="
    for value in values:
        if not value.startswith(prefix):
            continue
        match = re.search(r"(?:^|;)\s*expires=([^;]+)", value, re.IGNORECASE)
        if match:
            return parse_canonical_http_date(match.group(1), f"{name} expires")
    raise TestFailure(f"response did not contain an expires value for {name}")


def assert_expiry_delta(actual, reference, expected, context, tolerance=2):
    delta = (actual - reference).total_seconds()
    if abs(delta - expected) > tolerance:
        raise TestFailure(f"{context} expiry delta was {delta} seconds, expected about {expected}")


def assert_code(body, expected, context):
    if body.get("code") != expected:
        raise TestFailure(f"{context} returned code {body.get('code')!r}: {body.get('msg')!r}")


def run(base_url, secret, timeout):
    api_path = "/index/api/getApiList"

    for parameter_mode in ("query", "form", "json"):
        headers, body = request(
            base_url,
            api_path,
            {"secret": secret},
            timeout=timeout,
            parameter_mode=parameter_mode,
        )
        assert_code(body, 0, f"valid secret request ({parameter_mode})")
        if set_cookie_values(headers):
            raise TestFailure("valid secret authentication must not issue a cookie")
        if "cookie" in body:
            raise TestFailure("valid secret authentication must not return a challenge")

    headers, body = request(base_url, api_path, timeout=timeout)
    assert_code(body, -100, "missing credentials")
    challenge = body.get("cookie")
    if not challenge:
        raise TestFailure("missing credentials did not return a login challenge")
    challenge_cookies = set_cookie_values(headers)
    assert_canonical_expires(challenge_cookies)
    if extract_cookie(challenge_cookies, "ZLM_UNLOGIN") != challenge:
        raise TestFailure("ZLM_UNLOGIN header did not match the response challenge")
    assert_expiry_delta(
        cookie_expiry(challenge_cookies, "ZLM_UNLOGIN"),
        response_date(headers),
        60,
        "ZLM_UNLOGIN",
    )

    anonymous_cookie = f"ZLM_UNLOGIN={challenge}"
    headers, body = request(base_url, api_path, cookie=anonymous_cookie, timeout=timeout)
    assert_code(body, -100, "reused login challenge")
    if body.get("cookie") != challenge:
        raise TestFailure("server did not reuse the existing login challenge")
    if set_cookie_values(headers):
        raise TestFailure("reused login challenge unexpectedly issued another cookie")

    headers, body = request(
        base_url,
        api_path,
        {"secret": secret + "-invalid"},
        cookie=anonymous_cookie,
        timeout=timeout,
    )
    assert_code(body, -100, "invalid secret")
    if body.get("cookie") != challenge:
        raise TestFailure("invalid secret did not preserve the login challenge")
    if set_cookie_values(headers):
        raise TestFailure("invalid secret with an existing challenge issued another cookie")

    headers, body = request(
        base_url,
        api_path,
        {"secret": secret},
        cookie=anonymous_cookie,
        timeout=timeout,
    )
    assert_code(body, 0, "valid secret with an existing challenge")
    if set_cookie_values(headers) or "cookie" in body:
        raise TestFailure("valid secret with an existing challenge returned another challenge")

    digest = hashlib.md5(f"zlmediakit:{secret}:{challenge}".encode()).hexdigest()
    headers, body = request(
        base_url,
        "/index/api/login",
        {"digest": digest},
        cookie=anonymous_cookie,
        timeout=timeout,
    )
    assert_code(body, 0, "cookie login")
    login_cookies = set_cookie_values(headers)
    assert_canonical_expires(login_cookies)
    login_token = extract_cookie(login_cookies, "ZLM_LOGINED")
    if not login_token:
        raise TestFailure("successful cookie login did not issue ZLM_LOGINED")
    if extract_cookie(login_cookies, "ZLM_UNLOGIN") != challenge:
        raise TestFailure("successful cookie login did not expire the original ZLM_UNLOGIN")
    login_date = response_date(headers)
    assert_expiry_delta(cookie_expiry(login_cookies, "ZLM_LOGINED"), login_date, 24 * 60 * 60, "ZLM_LOGINED")
    assert_expiry_delta(cookie_expiry(login_cookies, "ZLM_UNLOGIN"), login_date, 0, "expired ZLM_UNLOGIN")

    login_cookie = f"ZLM_LOGINED={login_token}"
    headers, body = request(base_url, api_path, cookie=login_cookie, timeout=timeout)
    assert_code(body, 0, "cookie-only API request")
    if set_cookie_values(headers):
        raise TestFailure("valid login cookie unexpectedly issued another cookie")

    headers, body = request(
        base_url,
        api_path,
        {"secret": secret + "-invalid"},
        cookie=login_cookie,
        timeout=timeout,
    )
    assert_code(body, 0, "login cookie with invalid secret")
    if set_cookie_values(headers):
        raise TestFailure("valid login cookie with invalid secret issued another cookie")

    headers, body = request(base_url, "/index/api/logout", cookie=login_cookie, timeout=timeout)
    assert_code(body, 0, "cookie logout")
    logout_cookies = set_cookie_values(headers)
    assert_canonical_expires(logout_cookies)
    if extract_cookie(logout_cookies, "ZLM_LOGINED") != login_token:
        raise TestFailure("logout did not expire the original ZLM_LOGINED")
    logout_challenge = extract_cookie(logout_cookies, "ZLM_UNLOGIN")
    if not logout_challenge or body.get("cookie") != logout_challenge:
        raise TestFailure("logout did not return a replacement ZLM_UNLOGIN challenge")
    logout_date = response_date(headers)
    assert_expiry_delta(cookie_expiry(logout_cookies, "ZLM_LOGINED"), logout_date, 0, "expired ZLM_LOGINED")
    assert_expiry_delta(cookie_expiry(logout_cookies, "ZLM_UNLOGIN"), logout_date, 60, "logout ZLM_UNLOGIN")


def unused_tcp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def write_test_config(template, destination, port, secret):
    config = configparser.ConfigParser(interpolation=None, strict=False)
    config.optionxform = str
    with template.open("r", encoding="utf-8-sig") as stream:
        config.read_file(stream)

    values = {
        "api": {"apiDebug": "0", "secret": secret},
        "general": {"listen_ip": "127.0.0.1"},
        "http": {"port": str(port), "sslport": "0"},
        "rtmp": {"port": "0", "sslport": "0"},
        "rtp_proxy": {"port": "0"},
        "rtc": {
            "signalingPort": "0",
            "signalingSslPort": "0",
            "icePort": "0",
            "iceTcpPort": "0",
            "port": "0",
            "tcpPort": "0",
        },
        "srt": {"port": "0"},
        "rtsp": {"port": "0", "sslport": "0"},
        "shell": {"port": "0"},
        "onvif": {"port": "0"},
    }
    for section, options in values.items():
        if not config.has_section(section):
            config.add_section(section)
        for key, value in options.items():
            config.set(section, key, value)

    with destination.open("w", encoding="utf-8") as stream:
        config.write(stream, space_around_delimiters=False)


def wait_until_ready(process, base_url, secret, timeout, startup_timeout):
    deadline = time.monotonic() + startup_timeout
    last_error = None
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise TestFailure(f"MediaServer exited with status {process.returncode}")
        try:
            _, body = request(
                base_url,
                "/index/api/getApiList",
                {"secret": secret},
                timeout=timeout,
            )
            assert_code(body, 0, "MediaServer readiness probe")
            return
        except (OSError, TestFailure) as ex:
            last_error = ex
            time.sleep(0.1)
    raise TestFailure(f"MediaServer did not become ready: {last_error}")


def stop_process(process):
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def run_managed_server(server, config_template, certificate, timeout, startup_timeout):
    server = server.resolve()
    config_template = (config_template or server.with_name("config.ini")).resolve()
    certificate = (certificate or server.with_name("default.pem")).resolve()
    for path, description in (
        (server, "MediaServer"),
        (config_template, "config template"),
        (certificate, "certificate"),
    ):
        if not path.is_file():
            raise TestFailure(f"{description} does not exist: {path}")

    port = unused_tcp_port()
    secret = secrets.token_hex(24)
    base_url = f"http://127.0.0.1:{port}"
    with tempfile.TemporaryDirectory(prefix="zlm-http-auth-") as temp_dir_name:
        temp_dir = Path(temp_dir_name)
        config_path = temp_dir / "config.ini"
        output_path = temp_dir / "MediaServer.log"
        write_test_config(config_template, config_path, port, secret)

        with output_path.open("w+", encoding="utf-8") as output:
            process = subprocess.Popen(
                [
                    str(server),
                    "-c",
                    str(config_path),
                    "-s",
                    str(certificate),
                    "-t",
                    "1",
                    "--affinity",
                    "0",
                    "-l",
                    "4",
                    "--log-dir",
                    str(temp_dir / "log"),
                ],
                cwd=server.parent,
                stdout=output,
                stderr=subprocess.STDOUT,
                text=True,
            )
            failure = None
            stop_error = None
            try:
                wait_until_ready(process, base_url, secret, timeout, startup_timeout)
                run(base_url, secret, timeout)
            except (OSError, TestFailure) as ex:
                failure = ex
            finally:
                try:
                    stop_process(process)
                except (OSError, subprocess.SubprocessError) as ex:
                    stop_error = ex

            if failure or stop_error:
                output.flush()
                details = output_path.read_text(encoding="utf-8", errors="replace").splitlines()[-40:]
                message = str(failure or stop_error)
                if failure and stop_error:
                    message += f"; stopping MediaServer also failed: {stop_error}"
                if details:
                    message += "\nMediaServer output:\n" + "\n".join(details)
                raise TestFailure(message) from failure


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base_url", nargs="?", help="existing MediaServer base URL")
    parser.add_argument("--server", type=Path, help="start and test this MediaServer binary")
    parser.add_argument("--config", type=Path, help="config template for --server")
    parser.add_argument("--certificate", type=Path, help="certificate for --server")
    parser.add_argument(
        "--secret-env",
        default="ZLM_API_SECRET",
        help="environment variable containing the API secret (default: ZLM_API_SECRET)",
    )
    parser.add_argument("--timeout", type=float, default=5.0, help="request timeout in seconds")
    parser.add_argument("--startup-timeout", type=float, default=15.0, help="MediaServer startup timeout in seconds")
    args = parser.parse_args()

    if bool(args.base_url) == bool(args.server):
        parser.error("provide either base_url or --server")
    try:
        if args.server:
            run_managed_server(args.server, args.config, args.certificate, args.timeout, args.startup_timeout)
        else:
            secret = os.environ.get(args.secret_env)
            if not secret:
                parser.error(f"environment variable {args.secret_env} is empty or unset")
            run(args.base_url, secret, args.timeout)
    except (OSError, TestFailure) as ex:
        print(f"test_http_api_auth failed: {ex}", file=sys.stderr)
        return 1
    print("test_http_api_auth passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
