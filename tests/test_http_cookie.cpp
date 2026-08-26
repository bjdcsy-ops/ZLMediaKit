/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include <cstdlib>
#include <ctime>
#include <iostream>
#include <stdexcept>
#include <string>

#include "Http/HttpCookie.h"
#include "Http/HttpCookieManager.h"
#include "Http/HttpConst.h"

using namespace std;
using namespace mediakit;

namespace {

void expect(bool cond, const string &msg) {
    if (!cond) {
        throw runtime_error(msg);
    }
}

void testCookieExpiresUsesImfFixdate() {
    auto before = time(nullptr);
    auto cookie = HttpCookieManager::Instance().addCookie("TEST_SESSION", "test-user", 60);
    auto header = cookie->getCookie("/");

    auto expires_begin = header.find(";expires=");
    expect(expires_begin != string::npos, "Set-Cookie should contain expires");
    expires_begin += sizeof(";expires=") - 1;
    auto expires_end = header.find(';', expires_begin);
    expect(expires_end != string::npos, "expires should be followed by another attribute");

    auto expires = header.substr(expires_begin, expires_end - expires_begin);
    time_t expires_at;
    expect(HttpConst::parseHttpDate(expires, expires_at), "expires should be a valid HTTP date");
    expect(HttpConst::formatHttpDate(expires_at) == expires, "expires should use canonical IMF-fixdate");
    auto after = time(nullptr);
    expect(expires_at >= before + 60 && expires_at <= after + 60, "expires should preserve the cookie lifetime");

    auto prefix = string("TEST_SESSION=") + cookie->getCookie() + ";";
    expect(header.compare(0, prefix.size(), prefix) == 0, "Set-Cookie should contain the generated cookie");
    expect(header.size() >= 7 && header.compare(header.size() - 7, 7, ";path=/") == 0,
           "Set-Cookie should preserve the requested path");

    HttpCookieManager::Instance().delCookie(cookie);
    cookie.reset();
}

void testHttpDateFormattingAndValidation() {
    const string canonical = "Wed, 26 Aug 2026 05:43:02 GMT";
    time_t canonical_time;
    expect(HttpConst::parseHttpDate(canonical, canonical_time), "parser should accept IMF-fixdate");
    expect(HttpConst::formatHttpDate(canonical_time) == canonical, "formatter should emit exact IMF-fixdate");

    time_t legacy_time;
    expect(HttpConst::parseHttpDate("Wed, Aug 26 2026 05:43:02 GMT", legacy_time),
           "parser should accept legacy ZLMediaKit dates");
    expect(legacy_time == canonical_time, "legacy and canonical dates should describe the same instant");

    time_t ignored;
    expect(!HttpConst::parseHttpDate("Mon, 26 Aug 2026 05:43:02 GMT", ignored),
           "parser should reject an incorrect weekday");
    expect(HttpConst::parseCookieDate("Mon, 26 Aug 2026 05:43:02 GMT", ignored),
           "cookie parser should tolerate an incorrect weekday");
    expect(!HttpConst::parseHttpDate("Mon, 31 Feb 2026 05:43:02 GMT", ignored),
           "parser should reject an invalid calendar date");

    time_t february_28;
    time_t february_29;
    time_t march_1;
    expect(HttpConst::parseHttpDate("Mon, 28 Feb 2028 00:00:00 GMT", february_28),
           "parser should accept the day before leap day");
    expect(HttpConst::parseHttpDate("Tue, 29 Feb 2028 00:00:00 GMT", february_29),
           "parser should accept leap day");
    expect(HttpConst::parseHttpDate("Wed, 01 Mar 2028 00:00:00 GMT", march_1),
           "parser should accept the day after leap day");
    expect(february_29 - february_28 == 24 * 60 * 60 && march_1 - february_29 == 24 * 60 * 60,
           "leap-day timestamps should remain one day apart");
}

bool acceptsExpires(const string &expires, const string &server_date) {
    HttpCookie cookie;
    cookie.setHost("127.0.0.1");
    cookie.setPath("/");
    cookie.setKeyVal("TEST_SESSION", "value");
    cookie.setExpires(expires, server_date);
    return cookie;
}

void testCookieParserAcceptsCanonicalAndLegacyDates() {
    const string canonical_server_date = "Tue, 01 Jan 2030 00:00:00 GMT";
    const string legacy_server_date = "Tue, Jan 01 2030 00:00:00 GMT";
    expect(acceptsExpires("Tue, 01 Jan 2030 01:00:00 GMT", canonical_server_date),
           "client should accept IMF-fixdate");
    expect(acceptsExpires("Tue, 01 Jan 2030 01:00:00 GMT", legacy_server_date),
           "client should accept canonical expires with a legacy server date");
    expect(acceptsExpires("Tue, Jan 01 2030 01:00:00 GMT", legacy_server_date),
           "client should accept legacy ZLMediaKit dates");
    expect(acceptsExpires("Mon, 01 Jan 2030 01:00:00 GMT", canonical_server_date),
           "client should preserve legacy tolerance for an incorrect cookie weekday");
    expect(!acceptsExpires("not a date", canonical_server_date), "client should reject malformed expires values");
    expect(acceptsExpires(HttpConst::formatHttpDate(time(nullptr) + 60 * 60), "not a date"),
           "client should use absolute expires when the server date is malformed");
}

} // namespace

int main() {
    try {
        testCookieExpiresUsesImfFixdate();
        testHttpDateFormattingAndValidation();
        testCookieParserAcceptsCanonicalAndLegacyDates();
        cout << "test_http_cookie passed" << endl;
        return 0;
    } catch (const exception &ex) {
        cerr << "test_http_cookie failed: " << ex.what() << endl;
        return EXIT_FAILURE;
    }
}
