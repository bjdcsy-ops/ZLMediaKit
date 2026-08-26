/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include <cstdint>
#include "HttpCookie.h"
#include "HttpConst.h"

using namespace std;

namespace mediakit {

void HttpCookie::setPath(const string &path) {
    _path = path;
}

void HttpCookie::setHost(const string &host) {
    _host = host;
}

void HttpCookie::setExpires(const string &expires, const string &server_date) {
    time_t expires_at;
    if (!HttpConst::parseCookieDate(expires, expires_at)) {
        _expire = 0;
        return;
    }

    _expire = expires_at;
    time_t server_at;
    if (!server_date.empty() && HttpConst::parseHttpDate(server_date, server_at)) {
        auto adjusted = static_cast<int64_t>(time(nullptr)) + static_cast<int64_t>(expires_at) -
                        static_cast<int64_t>(server_at);
        auto converted = static_cast<time_t>(adjusted);
        _expire = static_cast<int64_t>(converted) == adjusted ? converted : 0;
    }
}

void HttpCookie::setKeyVal(const string &key, const string &val) {
    _key = key;
    _val = val;
}

HttpCookie::operator bool() {
    return !_host.empty() && !_key.empty() && !_val.empty() && (_expire > time(NULL));
}

const string &HttpCookie::getVal() const {
    return _val;
}

const string &HttpCookie::getKey() const {
    return _key;
}

HttpCookieStorage &HttpCookieStorage::Instance() {
    static HttpCookieStorage instance;
    return instance;
}

void HttpCookieStorage::set(const HttpCookie::Ptr &cookie) {
    lock_guard<mutex> lck(_mtx_cookie);
    if (!cookie || !(*cookie)) {
        return;
    }
    _all_cookie[cookie->_host][cookie->_path][cookie->_key] = cookie;
}

vector<HttpCookie::Ptr> HttpCookieStorage::get(const string &host, const string &path) {
    vector<HttpCookie::Ptr> ret(0);
    lock_guard<mutex> lck(_mtx_cookie);
    auto it = _all_cookie.find(host);
    if (it == _all_cookie.end()) {
        // 未找到该host相关记录  [AUTO-TRANSLATED:0655542a]
        // No record found for this host
        return ret;
    }
    // 遍历该host下所有path  [AUTO-TRANSLATED:94ca2180]
    // Traverse all paths under this host
    for (auto &pr : it->second) {
        if (path.find(pr.first) != 0) {
            // 这个path不匹配  [AUTO-TRANSLATED:3ec99732]
            // This path does not match
            continue;
        }
        // 遍历该path下的各个cookie  [AUTO-TRANSLATED:ceab9c83]
        // Traverse all cookies under this path
        for (auto it_cookie = pr.second.begin(); it_cookie != pr.second.end();) {
            if (!*(it_cookie->second)) {
                // 该cookie已经过期，移除之  [AUTO-TRANSLATED:52762286]
                // This cookie has expired, remove it
                it_cookie = pr.second.erase(it_cookie);
                continue;
            }
            // 保存有效cookie  [AUTO-TRANSLATED:bd875507]
            // Save valid cookies
            ret.emplace_back(it_cookie->second);
            ++it_cookie;
        }
    }
    return ret;
}

} /* namespace mediakit */
