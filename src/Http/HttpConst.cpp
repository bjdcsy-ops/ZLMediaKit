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
#include <cstdio>
#include <cstring>
#include "HttpConst.h"
#include "Common/Parser.h"
#include "Util/onceToken.h"

using namespace std;
using namespace toolkit;

namespace mediakit{

namespace {

const char *kWeekdays[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
const char *kMonths[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

int parseFixedDigits(const string &value, size_t offset, size_t count) {
    int result = 0;
    for (size_t index = 0; index < count; ++index) {
        auto ch = value[offset + index];
        if (ch < '0' || ch > '9') {
            return -1;
        }
        result = result * 10 + ch - '0';
    }
    return result;
}

int findToken(const string &value, size_t offset, const char *const *tokens, size_t count) {
    for (size_t index = 0; index < count; ++index) {
        if (value.compare(offset, 3, tokens[index]) == 0) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

bool isLeapYear(int year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int daysInMonth(int year, int month) {
    static const int days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return month == 2 && isLeapYear(year) ? 29 : days[month - 1];
}

int64_t daysFromCivil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned adjusted_month = month > 2 ? month - 3 : month + 9;
    const unsigned day_of_year = (153 * adjusted_month + 2) / 5 + day - 1;
    const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return static_cast<int64_t>(era) * 146097 + day_of_era - 719468;
}

bool parseHttpDateFields(
    const string &date, bool legacy_order, int &weekday, int &year, int &month, int &day,
    int &hour, int &minute, int &second) {
    if (date.size() != 29 || date[3] != ',' || date[4] != ' ' || date[11] != ' ' || date[16] != ' ' ||
        date[19] != ':' || date[22] != ':' || date[25] != ' ' || date.compare(26, 3, "GMT") != 0) {
        return false;
    }

    weekday = findToken(date, 0, kWeekdays, sizeof(kWeekdays) / sizeof(kWeekdays[0]));
    if (legacy_order) {
        if (date[8] != ' ') {
            return false;
        }
        month = findToken(date, 5, kMonths, sizeof(kMonths) / sizeof(kMonths[0])) + 1;
        day = parseFixedDigits(date, 9, 2);
    } else {
        if (date[7] != ' ') {
            return false;
        }
        day = parseFixedDigits(date, 5, 2);
        month = findToken(date, 8, kMonths, sizeof(kMonths) / sizeof(kMonths[0])) + 1;
    }
    year = parseFixedDigits(date, 12, 4);
    hour = parseFixedDigits(date, 17, 2);
    minute = parseFixedDigits(date, 20, 2);
    second = parseFixedDigits(date, 23, 2);
    return weekday >= 0 && year >= 1970 && month >= 1 && month <= 12 && day >= 1 &&
           day <= daysInMonth(year, month) && hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59 &&
           second >= 0 && second <= 59;
}

bool parseDate(const string &date, time_t &timestamp, bool validate_weekday) {
    int weekday;
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
    if (!parseHttpDateFields(date, false, weekday, year, month, day, hour, minute, second) &&
        !parseHttpDateFields(date, true, weekday, year, month, day, hour, minute, second)) {
        return false;
    }

    auto days = daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    if (validate_weekday) {
        auto expected_weekday = static_cast<int>((days + 4) % 7);
        if (expected_weekday < 0) {
            expected_weekday += 7;
        }
        if (weekday != expected_weekday) {
            return false;
        }
    }

    auto seconds = days * 24 * 60 * 60 + hour * 60 * 60 + minute * 60 + second;
    auto converted = static_cast<time_t>(seconds);
    if (static_cast<int64_t>(converted) != seconds) {
        return false;
    }
    timestamp = converted;
    return true;
}

} // namespace

string HttpConst::formatHttpDate(time_t timestamp) {
    struct tm utc_time = {};
#if defined(_WIN32)
    if (gmtime_s(&utc_time, &timestamp) != 0) {
        return "";
    }
#else
    if (!gmtime_r(&timestamp, &utc_time)) {
        return "";
    }
#endif
    auto year = utc_time.tm_year + 1900;
    if (utc_time.tm_wday < 0 || utc_time.tm_wday > 6 || utc_time.tm_mon < 0 || utc_time.tm_mon > 11 ||
        year < 0 || year > 9999) {
        return "";
    }

    char buffer[32];
    auto size = snprintf(
        buffer, sizeof(buffer), "%s, %02d %s %04d %02d:%02d:%02d GMT", kWeekdays[utc_time.tm_wday],
        utc_time.tm_mday, kMonths[utc_time.tm_mon], year, utc_time.tm_hour, utc_time.tm_min, utc_time.tm_sec);
    return size == 29 ? string(buffer, static_cast<size_t>(size)) : "";
}

bool HttpConst::parseHttpDate(const string &date, time_t &timestamp) {
    return parseDate(date, timestamp, true);
}

bool HttpConst::parseCookieDate(const string &date, time_t &timestamp) {
    return parseDate(date, timestamp, false);
}

const char *HttpConst::getHttpStatusMessage(int status) {
    switch (status) {
        case 100: return "Continue";
        case 101: return "Switching Protocol";
        case 102: return "Processing";
        case 103: return "Early Hints";
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 203: return "Non-Authoritative Information";
        case 204: return "No Content";
        case 205: return "Reset Content";
        case 206: return "Partial Content";
        case 207: return "Multi-Status";
        case 208: return "Already Reported";
        case 226: return "IM Used";
        case 300: return "Multiple Choice";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 305: return "Use Proxy";
        case 306: return "unused";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 402: return "Payment Required";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 406: return "Not Acceptable";
        case 407: return "Proxy Authentication Required";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 410: return "Gone";
        case 411: return "Length Required";
        case 412: return "Precondition Failed";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 415: return "Unsupported Media Type";
        case 416: return "Range Not Satisfiable";
        case 417: return "Expectation Failed";
        case 418: return "I'm a teapot";
        case 421: return "Misdirected Request";
        case 422: return "Unprocessable Entity";
        case 423: return "Locked";
        case 424: return "Failed Dependency";
        case 425: return "Too Early";
        case 426: return "Upgrade Required";
        case 428: return "Precondition Required";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 451: return "Unavailable For Legal Reasons";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        case 505: return "HTTP Version Not Supported";
        case 506: return "Variant Also Negotiates";
        case 507: return "Insufficient Storage";
        case 508: return "Loop Detected";
        case 510: return "Not Extended";
        case 511: return "Network Authentication Required";

        default:
        case 500: return "Internal Server Error";
    }
}

static const char *s_mime_src[][2] = {
        {"html", "text/html"},
        {"htm", "text/html"},
        {"shtml", "text/html"},
        {"css", "text/css"},
        {"xml", "text/xml"},
        {"gif", "image/gif"},
        {"jpeg", "image/jpeg"},
        {"jpg", "image/jpeg"},
        {"js", "application/javascript"},
        {"map", "application/javascript" },
        {"atom", "application/atom+xml"},
        {"rss", "application/rss+xml"},
        {"mml", "text/mathml"},
        {"txt", "text/plain"},
        {"jad", "text/vnd.sun.j2me.app-descriptor"},
        {"wml", "text/vnd.wap.wml"},
        {"htc", "text/x-component"},
        {"png", "image/png"},
        {"tif", "image/tiff"},
        {"tiff", "image/tiff"},
        {"wbmp", "image/vnd.wap.wbmp"},
        {"ico", "image/x-icon"},
        {"jng", "image/x-jng"},
        {"bmp", "image/x-ms-bmp"},
        {"svg", "image/svg+xml"},
        {"svgz", "image/svg+xml"},
        {"webp", "image/webp"},
        {"woff", "application/font-woff"},
        {"woff2","application/font-woff" },
        {"jar", "application/java-archive"},
        {"war", "application/java-archive"},
        {"ear", "application/java-archive"},
        {"json", "application/json"},
        {"hqx", "application/mac-binhex40"},
        {"doc", "application/msword"},
        {"pdf", "application/pdf"},
        {"ps", "application/postscript"},
        {"eps", "application/postscript"},
        {"ai", "application/postscript"},
        {"rtf", "application/rtf"},
        {"m3u8", "application/vnd.apple.mpegurl"},
        {"xls", "application/vnd.ms-excel"},
        {"eot", "application/vnd.ms-fontobject"},
        {"ppt", "application/vnd.ms-powerpoint"},
        {"wmlc", "application/vnd.wap.wmlc"},
        {"kml", "application/vnd.google-earth.kml+xml"},
        {"kmz", "application/vnd.google-earth.kmz"},
        {"7z", "application/x-7z-compressed"},
        {"cco", "application/x-cocoa"},
        {"jardiff", "application/x-java-archive-diff"},
        {"jnlp", "application/x-java-jnlp-file"},
        {"run", "application/x-makeself"},
        {"pl", "application/x-perl"},
        {"pm", "application/x-perl"},
        {"prc", "application/x-pilot"},
        {"pdb", "application/x-pilot"},
        {"rar", "application/x-rar-compressed"},
        {"rpm", "application/x-redhat-package-manager"},
        {"sea", "application/x-sea"},
        {"swf", "application/x-shockwave-flash"},
        {"sit", "application/x-stuffit"},
        {"tcl", "application/x-tcl"},
        {"tk", "application/x-tcl"},
        {"der", "application/x-x509-ca-cert"},
        {"pem", "application/x-x509-ca-cert"},
        {"crt", "application/x-x509-ca-cert"},
        {"xpi", "application/x-xpinstall"},
        {"xhtml", "application/xhtml+xml"},
        {"xspf", "application/xspf+xml"},
        {"zip", "application/zip"},
        {"bin", "application/octet-stream"},
        {"exe", "application/octet-stream"},
        {"dll", "application/octet-stream"},
        {"deb", "application/octet-stream"},
        {"dmg", "application/octet-stream"},
        {"iso", "application/octet-stream"},
        {"img", "application/octet-stream"},
        {"msi", "application/octet-stream"},
        {"msp", "application/octet-stream"},
        {"msm", "application/octet-stream"},
        {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
        {"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
        {"mid", "audio/midi"},
        {"midi", "audio/midi"},
        {"kar", "audio/midi"},
        {"mp3", "audio/mpeg"},
        {"ogg", "audio/ogg"},
        {"m4a", "audio/x-m4a"},
        {"ra", "audio/x-realaudio"},
        {"3gpp", "video/3gpp"},
        {"3gp", "video/3gpp"},
        {"ts", "video/mp2t"},
        {"mp4", "video/mp4"},
        {"m4s", "video/mp4"},
        {"mpeg", "video/mpeg"},
        {"mpg", "video/mpeg"},
        {"mov", "video/quicktime"},
        {"webm", "video/webm"},
        {"flv", "video/x-flv"},
        {"m4v", "video/x-m4v"},
        {"mng", "video/x-mng"},
        {"asx", "video/x-ms-asf"},
        {"asf", "video/x-ms-asf"},
        {"wmv", "video/x-ms-wmv"},
        {"avi", "video/x-msvideo"},
};

const string& HttpConst::getHttpContentType(const char *name) {
    const char *dot;
    dot = strrchr(name, '.');
    static StrCaseMap mapType;
    static onceToken token([&]() {
        for (unsigned int i = 0; i < sizeof(s_mime_src) / sizeof(s_mime_src[0]); ++i) {
            mapType.emplace(s_mime_src[i][0], s_mime_src[i][1]);
        }
    });
    static string defaultType = "text/plain";
    if (!dot) {
        return defaultType;
    }
    auto it = mapType.find(dot + 1);
    if (it == mapType.end()) {
        return defaultType;
    }
    return it->second;
}

}//namespace mediakit
