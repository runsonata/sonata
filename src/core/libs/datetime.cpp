// @sonata/datetime: dates, times and time zones, precise to the nanosecond.
//
//     local datetime = require("@sonata/datetime")
//
//     datetime.now()                              --> t, nsec
//     datetime.monotonic()                        --> seconds (only for measuring durations)
//     datetime.toFields(t, zone?, nsec?)          --> { year, month, day, hour, min, sec, nsec,
//                                                       wday (1 = Sunday), yday, isoYear, isoWeek,
//                                                       offset (seconds east of UTC), isdst, abbr }
//     datetime.fromFields(fields, zone?)          --> t, nsec  (missing fields = 1970-01-01 00:00:00;
//                                                              out-of-range ones roll over: month 13, day 0, ...)
//     datetime.parse(text, format?, zone?)        --> t, nsec | nil, error
//     datetime.format(t, format?, zone?, nsec?)   --> string
//     datetime.add(t, delta, zone?, nsec?)        --> t, nsec
//     datetime.startOf(t, unit, zone?, nsec?)     --> t, nsec
//     datetime.convert(value, from, to)           --> number
//     datetime.isLeapYear(year)                   --> boolean
//     datetime.daysInMonth(year, month)           --> number
//
// INSTANTS are Unix seconds (a number, fractions allowed). A double only keeps
// ~0.2 microseconds near today, so everything that PRODUCES an instant returns
// a second value, its exact nanosecond part (0..999999999). Pass it back as the
// LAST argument ("nsec") of the functions that accept one to stay exact. When
// "nsec" is given, the fractional part of "t" is ignored.
//
// ZONES ("zone", default UTC) can be:
//   - a number: fixed offset in seconds east of UTC         (19800)
//   - "UTC" | "Z" | "+05:30" | "-0800" | "UTC+2"            (ISO sign: east is positive)
//   - an IANA name from the built-in table (kZones below)   ("Europe/Paris")
//   - a POSIX TZ string (west is positive!)                 ("EST5EDT,M3.2.0,M11.1.0", "<+03>-3")
// Only CURRENT rules are modelled: no historical transitions, no leap seconds.
// Local times in a DST gap move forward by the gap; ambiguous ones pick the earlier instant.
//
// FORMAT / PARSE tokens (parse accepts the same ones, minus the output-only ones):
//   %Y %y %m %d %e %H %I %M %S %p %j %a %A %b %B %h   calendar fields (%-d = no padding)
//   %u (1-7, Mon=1) %w (0-6, Sun=0) %V %G (ISO week / year) %s (epoch seconds)
//   %N (9 digits) %3N %6N %L (3 digits) %f (".123", trailing zeros trimmed, empty if 0)
//   %z (+0530) %:z (+05:30) %::z (+05:30:15) %Z (abbreviation)
//   %F %T %D %R %c (composites) %n %t %%
// Presets (instead of a format string): "iso" (default: RFC 3339; for parse it
// auto-detects ISO 8601 / RFC 3339 / RFC 2822 / HTTP dates), "rfc2822",
// "http" (use zone UTC), "date", "time", "ctime".
//
// delta (add): { years, months, weeks, days, hours, minutes, seconds, nsec }.
// Calendar units move local wall-clock time (DST-safe, day clamped: Jan 31 + 1 month =
// Feb 28/29); hours/minutes/seconds (fractions ok) and nsec move the exact instant.
// startOf units: year, month, week (Monday), day, hour, minute, second.
// convert scales: unix, unixms, unixus, julian, mjd, excel, filetime, dotnet, ntp, cocoa.
//

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

#include <sonata/core/library.hpp>
#include "lua.h"
#include "lualib.h"

#include "libs.hpp"

namespace sonata::lib::libs {

namespace {

// Same rule as in example.cpp: bindings validate ALL arguments first, and only
// then create C++ objects. Everything before the std::string in format() is
// trivially destructible, so an argument error never skips a destructor.

using i64 = std::int64_t;

constexpr i64 kDay = 86400;
constexpr i64 kBillion = 1000000000;
constexpr i64 kMaxSec = 10000000000000; // |t| limit (~317k years); keeps every i64 computation safe

constexpr const char* kMonths[] = {"January", "February", "March",     "April",   "May",      "June",
                                   "July",    "August",   "September", "October", "November", "December"};
constexpr const char* kDays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

// calendar math //////////////////////////////////////////////////

i64 floorDiv(i64 a, i64 b) {
    const i64 q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

i64 floorMod(i64 a, i64 b) { return a - floorDiv(a, b) * b; }

bool isLeap(i64 y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }

int daysInMonth(i64 y, int m) {
    constexpr int kLengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && isLeap(y) ? 29 : kLengths[m - 1];
}

// Proleptic Gregorian <-> days since 1970-01-01 (Howard Hinnant's algorithms).
i64 daysFromCivil(i64 y, int m, int d) {
    y -= m <= 2;
    const i64 era = floorDiv(y, 400);
    const i64 yoe = y - era * 400;
    const i64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const i64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void civilFromDays(i64 z, i64& y, int& m, int& d) {
    z += 719468;
    const i64 era = floorDiv(z, 146097);
    const i64 doe = z - era * 146097;
    const i64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const i64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const i64 mp = (5 * doy + 2) / 153;
    d = int(doy - (153 * mp + 2) / 5 + 1);
    m = int(mp < 10 ? mp + 3 : mp - 9);
    y = yoe + era * 400 + (m <= 2);
}

// Local "seconds since 1970-01-01 00:00 wall clock". Out-of-range fields roll over.
i64 localSeconds(i64 year, i64 month, i64 day, i64 h, i64 mi, i64 s) {
    const i64 m0 = year * 12 + (month - 1);
    const i64 first = daysFromCivil(floorDiv(m0, 12), int(floorMod(m0, 12)) + 1, 1);
    return (first + day - 1) * kDay + h * 3600 + mi * 60 + s;
}

int weeksInYear(i64 y) {
    const auto p = [](i64 v) { return floorMod(v + floorDiv(v, 4) - floorDiv(v, 100) + floorDiv(v, 400), 7); };
    return (p(y) == 4 || p(y - 1) == 3) ? 53 : 52;
}

// text cursor //////////////////////////////////////////////

bool ieq(const char* a, const char* b, std::size_t n) {
    for (; n != 0; --n, ++a, ++b) {
        if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) {
            return false;
        }
    }
    return true;
}

struct Cursor {
    const char* p;
    const char* end;

    bool eof() const { return p >= end; }
    bool atDigit() const { return !eof() && std::isdigit(static_cast<unsigned char>(*p)); }

    bool eat(char c) {
        if (!eof() && *p == c) {
            ++p;
            return true;
        }
        return false;
    }

    // Case-insensitive literal.
    bool word(const char* w, std::size_t n) {
        if (std::size_t(end - p) < n || !ieq(p, w, n)) return false;
        p += n;
        return true;
    }

    bool digits(int minD, int maxD, i64& out, int* count = nullptr) {
        int n = 0;
        i64 v = 0;
        while (n < maxD && atDigit()) {
            v = v * 10 + (*p++ - '0');
            ++n;
        }
        if (count) *count = n;
        out = v;
        return n >= minD;
    }

    // Optional sign; returns -1 for '-', else +1.
    int sign() {
        if (eat('-')) return -1;
        eat('+');
        return 1;
    }

    // hh[:mm[:ss]] or hhmm[ss] -> seconds.
    bool hms(i64& sec) {
        i64 h = 0, m = 0, s = 0;
        if (!digits(1, 2, h)) return false;
        if (eat(':')) {
            if (!digits(2, 2, m)) return false;
            if (eat(':') && !digits(2, 2, s)) return false;
        } else if (atDigit()) {
            if (!digits(2, 2, m)) return false;
            if (atDigit() && !digits(2, 2, s)) return false;
        }
        sec = h * 3600 + m * 60 + s;
        return true;
    }

    // 1-9 digits scaled to nanoseconds.
    bool fraction(int& nsec) {
        i64 v = 0;
        int n = 0;
        if (!digits(1, 9, v, &n)) return false;
        while (n++ < 9) v *= 10;
        nsec = int(v);
        return true;
    }
};

// zones //////////////////////////////////////////////////////////////////
//
// A zone is "standard offset + optional DST rule pair", i.e. a parsed POSIX TZ
// string. To get historical accuracy, replace offsetAt()/localToUtc() with a
// tzdata-backed implementation; nothing else depends on how zones work.

struct Rule {
    char kind = 'M'; // 'M' = Mm.w.d, 'J' = Jn (1..365, no Feb 29), 'N' = n (0..365)
    int month = 0, week = 0, day = 0, n = 0;
    int time = 7200; // seconds after local midnight (may be negative or > 24h)
};

struct Zone {
    int stdOff = 0, dstOff = 0; // seconds east of UTC
    bool dst = false;
    char stdAbbr[8] = "UTC";
    char dstAbbr[8] = "";
    Rule start, end;
};

struct Off {
    int offset;
    bool dst;
};

// Local seconds (since the epoch, wall clock) at which "r" fires in year y.
i64 ruleLocalSeconds(const Rule& r, i64 y) {
    i64 day;
    if (r.kind == 'M') {
        const i64 first = daysFromCivil(y, r.month, 1);
        const int firstWday = int(floorMod(first + 4, 7));
        int dom = 1 + (r.day - firstWday + 7) % 7 + (r.week - 1) * 7;
        while (dom > daysInMonth(y, r.month)) dom -= 7; // week 5 = last
        day = first + dom - 1;
    } else {
        day = daysFromCivil(y, 1, 1) + r.n - (r.kind == 'J' ? 1 : 0);
        if (r.kind == 'J' && r.n >= 60 && isLeap(y)) ++day;
    }
    return day * kDay + r.time;
}

Off offsetAt(const Zone& z, i64 utc) {
    if (!z.dst) return {z.stdOff, false};

    i64 y;
    int m, d;
    civilFromDays(floorDiv(utc + z.stdOff, kDay), y, m, d);

    const i64 s = ruleLocalSeconds(z.start, y) - z.stdOff; // DST begins (UTC)
    const i64 e = ruleLocalSeconds(z.end, y) - z.dstOff;   // DST ends (UTC)
    const bool in = s < e ? (utc >= s && utc < e) : (utc < e || utc >= s); // s > e: southern hemisphere
    return in ? Off{z.dstOff, true} : Off{z.stdOff, false};
}

i64 localToUtc(const Zone& z, i64 local) {
    const int offsets[2] = {z.stdOff, z.dstOff};
    i64 best = 0;
    bool found = false;

    for (int i = 0; i < (z.dst ? 2 : 1); ++i) {
        const i64 utc = local - offsets[i];
        if (offsetAt(z, utc).offset == offsets[i] && (!found || utc < best)) { // overlap: earlier instant wins
            best = utc;
            found = true;
        }
    }

    return found ? best : local - std::min(z.stdOff, z.dstOff); // gap: use the pre-transition offset
}

void fixedZone(Zone& z, int off) {
    z.stdOff = z.dstOff = off;
    z.dst = false;
    if (off == 0) {
        std::memcpy(z.stdAbbr, "UTC", 4);
        return;
    }
    const int a = off < 0 ? -off : off;
    char buf[40];
    std::snprintf(buf, sizeof buf, "%c%02d%02d%02d", off < 0 ? '-' : '+', a / 3600, a / 60 % 60, a % 60);
    buf[a % 3600 == 0 ? 3 : a % 60 == 0 ? 5 : 7] = '\0';
    std::memcpy(z.stdAbbr, buf, 8);
}

bool zoneName(Cursor& c, char (&out)[8]) {
    const char* start = c.p;
    std::size_t len;
    if (c.eat('<')) {
        start = c.p;
        while (!c.eof() && *c.p != '>') ++c.p;
        if (c.eof()) return false;
        len = std::size_t(c.p - start);
        ++c.p;
    } else {
        while (!c.eof() && std::isalpha(static_cast<unsigned char>(*c.p))) ++c.p;
        len = std::size_t(c.p - start);
        if (len < 3) return false;
    }
    if (len == 0 || len > 7) return false;
    std::memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

bool parseRule(Cursor& c, Rule& r) {
    i64 v = 0, w = 0, d = 0;
    if (c.eat('M')) {
        if (!c.digits(1, 2, v) || !c.eat('.') || !c.digits(1, 1, w) || !c.eat('.') || !c.digits(1, 1, d)) return false;
        if (v < 1 || v > 12 || w < 1 || w > 5 || d > 6) return false;
        r.kind = 'M';
        r.month = int(v);
        r.week = int(w);
        r.day = int(d);
    } else {
        r.kind = c.eat('J') ? 'J' : 'N';
        if (!c.digits(1, 3, v) || v > 365 || (r.kind == 'J' && v < 1)) return false;
        r.n = int(v);
    }
    r.time = 7200;
    if (c.eat('/')) {
        const int sg = c.sign();
        i64 t = 0;
        if (!c.hms(t)) return false;
        r.time = int(sg * t);
    }
    return true;
}

// std offset [dst [offset] [,start[/time],end[/time]]]   (POSIX: positive offset = WEST of UTC)
bool parsePosix(std::string_view s, Zone& z) {
    Cursor c{s.data(), s.data() + s.size()};
    i64 t = 0;
    if (!zoneName(c, z.stdAbbr)) return false;
    int sg = c.sign();
    if (!c.hms(t)) return false;
    z.stdOff = z.dstOff = int(-sg * t);
    z.dst = false;

    if (!c.eof()) {
        if (!zoneName(c, z.dstAbbr)) return false;
        z.dst = true;
        z.dstOff = z.stdOff + 3600;
        if (!c.eof() && *c.p != ',') {
            sg = c.sign();
            if (!c.hms(t)) return false;
            z.dstOff = int(-sg * t);
        }
        if (c.eat(',')) {
            if (!parseRule(c, z.start) || !c.eat(',') || !parseRule(c, z.end)) return false;
        } else {
            z.start = Rule{'M', 3, 2, 0, 0, 7200}; // US rules when none are given
            z.end = Rule{'M', 11, 1, 0, 0, 7200};
        }
    }
    return c.eof();
}

struct ZoneDef {
    std::string_view names; // space separated
    std::string_view rule;
};

constexpr ZoneDef kZones[] = {
    {"UTC UT GMT Z Zulu UCT Etc/UTC Etc/GMT Etc/UCT Universal", "UTC0"},
    {"Africa/Accra Africa/Abidjan Africa/Dakar Atlantic/Reykjavik", "GMT0"},
    // Americas
    {"America/New_York America/Detroit America/Toronto America/Nassau", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Chicago America/Winnipeg", "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Denver America/Edmonton America/Boise", "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Los_Angeles America/Vancouver America/Tijuana", "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Anchorage", "AKST9AKDT,M3.2.0,M11.1.0"},
    {"America/Halifax", "AST4ADT,M3.2.0,M11.1.0"},
    {"America/St_Johns", "NST3:30NDT,M3.2.0,M11.1.0"},
    {"America/Phoenix", "MST7"},
    {"Pacific/Honolulu", "HST10"},
    {"America/Mexico_City America/Guatemala America/Costa_Rica America/Regina", "CST6"},
    {"America/Panama America/Jamaica", "EST5"},
    {"America/Bogota America/Lima America/Guayaquil", "<-05>5"},
    {"America/Caracas America/La_Paz America/Manaus", "<-04>4"},
    {"America/Sao_Paulo America/Argentina/Buenos_Aires America/Montevideo America/Bahia", "<-03>3"},
    {"America/Santiago", "<-04>4<-03>,M9.1.6/24,M4.1.6/24"},
    // Europe
    {"Europe/London", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe/Dublin", "GMT0IST,M3.5.0/1,M10.5.0"},
    {"Europe/Lisbon Atlantic/Canary Atlantic/Faroe", "WET0WEST,M3.5.0/1,M10.5.0"},
    {"Europe/Paris Europe/Berlin Europe/Madrid Europe/Rome Europe/Amsterdam Europe/Brussels Europe/Vienna "
     "Europe/Zurich Europe/Stockholm Europe/Oslo Europe/Copenhagen Europe/Warsaw Europe/Prague Europe/Budapest "
     "Europe/Belgrade Europe/Luxembourg Europe/Malta Africa/Ceuta",
     "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Athens Europe/Helsinki Europe/Kyiv Europe/Kiev Europe/Bucharest Europe/Sofia Europe/Riga "
     "Europe/Vilnius Europe/Tallinn Asia/Nicosia",
     "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Moscow", "MSK-3"},
    {"Europe/Istanbul Europe/Minsk Asia/Riyadh Asia/Baghdad Asia/Qatar Asia/Kuwait", "<+03>-3"},
    // Africa
    {"Africa/Cairo", "EET-2EEST,M4.5.5/0,M10.5.4/24"},
    {"Africa/Algiers Africa/Tunis", "CET-1"},
    {"Africa/Lagos Africa/Kinshasa Africa/Luanda", "WAT-1"},
    {"Africa/Johannesburg", "SAST-2"},
    {"Africa/Harare Africa/Maputo Africa/Lusaka Africa/Khartoum Africa/Kigali", "CAT-2"},
    {"Africa/Nairobi Africa/Addis_Ababa Africa/Dar_es_Salaam Africa/Kampala", "EAT-3"},
    // Asia
    {"Asia/Jerusalem Asia/Tel_Aviv", "IST-2IDT,M3.4.4/26,M10.5.0"},
    {"Asia/Tehran", "<+0330>-3:30"},
    {"Asia/Dubai Asia/Muscat Asia/Baku Asia/Tbilisi Asia/Yerevan", "<+04>-4"},
    {"Asia/Kabul", "<+0430>-4:30"},
    {"Asia/Karachi", "PKT-5"},
    {"Asia/Tashkent Asia/Yekaterinburg", "<+05>-5"},
    {"Asia/Kolkata Asia/Calcutta", "IST-5:30"},
    {"Asia/Kathmandu", "<+0545>-5:45"},
    {"Asia/Dhaka", "<+06>-6"},
    {"Asia/Bangkok Asia/Ho_Chi_Minh Asia/Phnom_Penh", "<+07>-7"},
    {"Asia/Jakarta", "WIB-7"},
    {"Asia/Shanghai Asia/Taipei Asia/Macau", "CST-8"},
    {"Asia/Hong_Kong", "HKT-8"},
    {"Asia/Singapore Asia/Kuala_Lumpur Asia/Brunei", "<+08>-8"},
    {"Asia/Manila", "PST-8"},
    {"Asia/Tokyo", "JST-9"},
    {"Asia/Seoul Asia/Pyongyang", "KST-9"},
    // Oceania
    {"Australia/Sydney Australia/Melbourne Australia/Canberra Australia/Hobart", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Australia/Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {"Australia/Brisbane", "AEST-10"},
    {"Australia/Darwin", "ACST-9:30"},
    {"Australia/Perth", "AWST-8"},
    {"Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
    {"Pacific/Fiji", "<+12>-12"},
};

bool findZone(std::string_view name, std::string_view& rule) {
    for (const ZoneDef& def : kZones) {
        for (std::size_t at = def.names.find(name); at != std::string_view::npos; at = def.names.find(name, at + 1)) {
            const bool left = at == 0 || def.names[at - 1] == ' ';
            const bool right = at + name.size() == def.names.size() || def.names[at + name.size()] == ' ';
            if (left && right) {
                rule = def.rule;
                return true;
            }
        }
    }
    return false;
}

bool parseZone(std::string_view s, Zone& z) {
    std::string_view rule;
    if (s.empty()) return false;
    if (findZone(s, rule)) return parsePosix(rule, z);

    std::string_view r = s; // "UTC+2" / "GMT-05:00" are ISO-style (east positive), unlike real POSIX strings
    if (r.size() > 3 && (r.substr(0, 3) == "UTC" || r.substr(0, 3) == "GMT") && (r[3] == '+' || r[3] == '-')) {
        r.remove_prefix(3);
    }
    if (r[0] == '+' || r[0] == '-') {
        Cursor c{r.data(), r.data() + r.size()};
        const int sg = c.sign();
        i64 t = 0;
        if (!c.hms(t) || !c.eof() || t >= kDay) return false;
        fixedZone(z, int(sg * t));
        return true;
    }
    return parsePosix(s, z);
}

// zone argument: nil -> UTC, number -> fixed offset, string -> name / offset / POSIX.
Zone readZone(lua_State* L, int idx) {
    Zone z;
    if (lua_isnoneornil(L, idx)) return z;

    if (lua_type(L, idx) == LUA_TNUMBER) {
        const double v = lua_tonumber(L, idx);
        luaL_argcheck(L, std::fabs(v) < double(kDay) && v == std::floor(v), idx, "offset must be whole seconds within +-24h");
        fixedZone(z, int(v));
        return z;
    }

    std::size_t len = 0;
    const char* s = luaL_checklstring(L, idx, &len);
    luaL_argcheck(L, parseZone(std::string_view(s, len), z), idx, "unknown time zone");
    return z;
}

// instants & fields /////////////////////////////////////////////

struct Instant {
    i64 sec;
    int nsec; // 0..999999999
};

// t (+ optional exact nsec at nsIdx) -> Instant.
Instant readInstant(lua_State* L, int tIdx, int nsIdx) {
    const double t = luaL_checknumber(L, tIdx);
    luaL_argcheck(L, std::fabs(t) <= double(kMaxSec), tIdx, "time out of range");

    const double whole = std::floor(t);
    Instant in{i64(whole), 0};

    if (!lua_isnoneornil(L, nsIdx)) {
        const double n = luaL_checknumber(L, nsIdx);
        luaL_argcheck(L, n >= 0 && n < double(kBillion) && n == std::floor(n), nsIdx, "nsec must be an integer in [0, 999999999]");
        in.nsec = int(n);
    } else {
        in.nsec = int(std::llround((t - whole) * double(kBillion)));
        if (in.nsec == kBillion) {
            in.nsec = 0;
            ++in.sec;
        }
    }
    return in;
}

// Carries nanoseconds into seconds and range-checks.
Instant make(lua_State* L, i64 sec, i64 nsec) {
    sec += floorDiv(nsec, kBillion);
    if (sec > kMaxSec || sec < -kMaxSec) luaL_error(L, "time out of range");
    return {sec, int(floorMod(nsec, kBillion))};
}

int pushInstant(lua_State* L, const Instant& in) {
    lua_pushnumber(L, double(in.sec) + double(in.nsec) / double(kBillion));
    lua_pushnumber(L, in.nsec);
    return 2;
}

struct Fields {
    i64 year, isoYear, epoch;
    int month, day, hour, min, sec, nsec;
    int wday; // 0 = Sunday
    int yday, isoWeek, offset;
    bool dst;
    char abbr[8];
};

Fields makeFields(const Zone& z, const Instant& in) {
    Fields f{};
    const Off o = offsetAt(z, in.sec);
    const i64 local = in.sec + o.offset;
    const i64 days = floorDiv(local, kDay);
    const i64 sod = local - days * kDay;

    civilFromDays(days, f.year, f.month, f.day);
    f.hour = int(sod / 3600);
    f.min = int(sod / 60 % 60);
    f.sec = int(sod % 60);
    f.nsec = in.nsec;
    f.epoch = in.sec;
    f.offset = o.offset;
    f.dst = o.dst;
    std::memcpy(f.abbr, o.dst ? z.dstAbbr : z.stdAbbr, sizeof f.abbr);
    f.wday = int(floorMod(days + 4, 7));
    f.yday = int(days - daysFromCivil(f.year, 1, 1)) + 1;

    // ISO 8601 week: weeks start Monday, week 1 contains the year's first Thursday.
    int week = (f.yday - (f.wday == 0 ? 7 : f.wday) + 10) / 7;
    f.isoYear = f.year;
    if (week < 1) {
        f.isoYear = f.year - 1;
        week = weeksInYear(f.isoYear);
    } else if (week > weeksInYear(f.year)) {
        f.isoYear = f.year + 1;
        week = 1;
    }
    f.isoWeek = week;
    return f;
}

// Integer-valued (or, with integer = false, any finite) number field of the table at idx, else "def".
double fieldNum(lua_State* L, int idx, const char* key, double def, bool integer = true) {
    lua_getfield(L, idx, key);
    double v = def;
    if (!lua_isnoneornil(L, -1)) {
        if (lua_type(L, -1) != LUA_TNUMBER) luaL_error(L, "field '%s' must be a number", key);
        v = lua_tonumber(L, -1);
        if (!(std::fabs(v) <= 1e12) || (integer && v != std::floor(v))) {
            luaL_error(L, "field '%s' must be %s within +-1e12", key, integer ? "a whole number" : "a number");
        }
    }
    lua_pop(L, 1);
    return v;
}

// formats /////////////////////////////////////////////////////////////////

struct Parsed {
    int year = 1970, month = 1, day = 1, hour = 0, min = 0, sec = 0, nsec = 0, yday = 0;
    int pm = -1; // -1 unset, 0 AM, 1 PM
    bool hour12 = false;
    bool hasOffset = false;
    int offset = 0;
    bool hasEpoch = false;
    i64 epoch = 0;
};

constexpr std::pair<std::string_view, std::string_view> kPresets[] = {
    {"rfc2822", "%a, %d %b %Y %H:%M:%S %z"},
    {"http", "%a, %d %b %Y %H:%M:%S GMT"},
    {"date", "%F"},
    {"time", "%T"},
    {"ctime", "%a %b %e %T %Y"},
};

std::string_view presetOf(std::string_view name) {
    for (const auto& [key, value] : kPresets) {
        if (key == name) return value;
    }
    return name;
}

std::string_view composite(char c) {
    switch (c) {
    case 'F': return "%Y-%m-%d";
    case 'T': return "%H:%M:%S";
    case 'D': return "%m/%d/%y";
    case 'R': return "%H:%M";
    case 'c': return "%a %b %e %T %Y";
    default: return {};
    }
}

// output /////////////////////////////////////////////////////

void putNum(std::string& out, i64 v, int width, char pad = '0') {
    char buf[24];
    const int n = std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v < 0 ? -v : v));
    if (v < 0) out += '-';
    out.append(std::size_t(std::max(0, width - n)), pad);
    out.append(buf, std::size_t(n));
}

void formatInto(std::string& out, std::string_view fmt, const Fields& f) {
    for (std::size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%' || i + 1 >= fmt.size()) {
            out += fmt[i];
            continue;
        }

        bool nopad = false;
        int colons = 0, width = 0;
        std::size_t j = i + 1;
        for (; j < fmt.size(); ++j) { // flags: '-' no padding, ':' colons, digits = width
            if (fmt[j] == '-') nopad = true;
            else if (fmt[j] == ':') ++colons;
            else if (std::isdigit(static_cast<unsigned char>(fmt[j]))) width = width * 10 + (fmt[j] - '0');
            else break;
        }
        if (j >= fmt.size()) {
            out.append(fmt.substr(i));
            break;
        }

        const char c = fmt[j];
        i = j;
        const auto num = [&](i64 v, int w) { putNum(out, v, nopad ? 0 : w); };

        switch (c) {
        case 'Y': num(f.year, 4); break;
        case 'y': num(floorMod(f.year, 100), 2); break;
        case 'm': num(f.month, 2); break;
        case 'd': num(f.day, 2); break;
        case 'e': putNum(out, f.day, nopad ? 0 : 2, ' '); break;
        case 'H': num(f.hour, 2); break;
        case 'I': num(f.hour % 12 == 0 ? 12 : f.hour % 12, 2); break;
        case 'M': num(f.min, 2); break;
        case 'S': num(f.sec, 2); break;
        case 'j': num(f.yday, 3); break;
        case 'u': num(f.wday == 0 ? 7 : f.wday, 1); break;
        case 'w': num(f.wday, 1); break;
        case 'V': num(f.isoWeek, 2); break;
        case 'G': num(f.isoYear, 4); break;
        case 's': num(f.epoch, 1); break;
        case 'p': out += f.hour < 12 ? "AM" : "PM"; break;
        case 'a': out.append(kDays[f.wday], 3); break;
        case 'A': out += kDays[f.wday]; break;
        case 'b':
        case 'h': out.append(kMonths[f.month - 1], 3); break;
        case 'B': out += kMonths[f.month - 1]; break;
        case 'Z': out += f.abbr; break;
        case 'z': {
            const int a = f.offset < 0 ? -f.offset : f.offset;
            out += f.offset < 0 ? '-' : '+';
            putNum(out, a / 3600, 2);
            if (colons) out += ':';
            putNum(out, a / 60 % 60, 2);
            if (colons >= 2 || a % 60 != 0) {
                if (colons) out += ':';
                putNum(out, a % 60, 2);
            }
            break;
        }
        case 'N':
        case 'L':
        case 'f': {
            char buf[16];
            std::snprintf(buf, sizeof buf, "%09d", f.nsec);
            std::size_t n = c == 'L' ? 3 : (width > 0 && width < 9 ? std::size_t(width) : 9);
            if (c == 'f') {
                for (n = 9; n > 0 && buf[n - 1] == '0'; --n) {}
                if (n > 0) out += '.';
            }
            out.append(buf, n);
            break;
        }
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case '%': out += '%'; break;
        default: {
            const std::string_view sub = composite(c);
            if (!sub.empty()) {
                formatInto(out, sub, f);
            } else {
                out += '%';
                out += c;
            }
        }
        }
    }
}

// input ////////////////////////////////////////////////////////////////

struct NumTok {
    char tok;
    int digits;
    int Parsed::*dest;
};

constexpr NumTok kNums[] = {
    {'Y', 4, &Parsed::year}, {'y', 2, &Parsed::year}, {'m', 2, &Parsed::month}, {'d', 2, &Parsed::day},
    {'e', 2, &Parsed::day},  {'H', 2, &Parsed::hour}, {'I', 2, &Parsed::hour},  {'M', 2, &Parsed::min},
    {'S', 2, &Parsed::sec},  {'j', 3, &Parsed::yday},
};

const char* parseInto(Cursor& c, std::string_view fmt, Parsed& p);

bool matchName(Cursor& c, const char* const* names, int count, int& idx) {
    for (int i = 0; i < count; ++i) {
        if (c.word(names[i], std::strlen(names[i])) || c.word(names[i], 3)) {
            idx = i;
            return true;
        }
    }
    return false;
}

// Z | UTC | GMT | UT | +hh[:mm[:ss]] | +hhmm
bool parseOffset(Cursor& c, Parsed& p) {
    i64 t = 0;
    int sg = 1;
    if (!(c.word("Z", 1) || c.word("UTC", 3) || c.word("GMT", 3) || c.word("UT", 2))) {
        if (c.eof() || (*c.p != '+' && *c.p != '-')) return false;
        sg = c.sign();
        if (!c.hms(t) || t >= kDay) return false;
    }
    p.hasOffset = true;
    p.offset = int(sg * t);
    return true;
}

const char* parseToken(Cursor& c, char t, Parsed& p) {
    for (const NumTok& n : kNums) {
        if (n.tok != t) continue;
        if (t == 'e') {
            while (c.eat(' ')) {}
        }
        i64 v = 0;
        if (!c.digits(1, n.digits, v)) return "expected a number";
        p.*n.dest = int(v);
        if (t == 'y') p.year += v < 69 ? 2000 : 1900;
        if (t == 'I') p.hour12 = true;
        return nullptr;
    }

    int idx = 0;
    switch (t) {
    case 'a':
    case 'A': return matchName(c, kDays, 7, idx) ? nullptr : "expected a weekday name";
    case 'b':
    case 'B':
    case 'h':
        if (!matchName(c, kMonths, 12, idx)) return "expected a month name";
        p.month = idx + 1;
        return nullptr;
    case 'p':
        if (c.word("AM", 2)) p.pm = 0;
        else if (c.word("PM", 2)) p.pm = 1;
        else return "expected AM or PM";
        return nullptr;
    case 'f':
    case 'N':
    case 'L': return c.fraction(p.nsec) ? nullptr : "expected fractional digits";
    case 's': {
        const bool neg = c.eat('-');
        i64 v = 0;
        if (!c.digits(1, 15, v)) return "expected epoch seconds";
        p.hasEpoch = true;
        p.epoch = neg ? -v : v;
        return nullptr;
    }
    case 'z': return parseOffset(c, p) ? nullptr : "bad UTC offset";
    case 'Z': // abbreviations are ambiguous, so they are skipped, not interpreted
        while (!c.eof() && std::isalpha(static_cast<unsigned char>(*c.p))) ++c.p;
        return nullptr;
    case '%': return c.eat('%') ? nullptr : "expected '%'";
    default: {
        const std::string_view sub = composite(t);
        return sub.empty() ? "unsupported format token" : parseInto(c, sub, p);
    }
    }
}

const char* parseInto(Cursor& c, std::string_view fmt, Parsed& p) {
    for (std::size_t i = 0; i < fmt.size(); ++i) {
        const char ch = fmt[i];
        if (std::isspace(static_cast<unsigned char>(ch))) { // whitespace matches any amount of whitespace
            while (!c.eof() && std::isspace(static_cast<unsigned char>(*c.p))) ++c.p;
        } else if (ch != '%' || i + 1 >= fmt.size()) {
            if (!c.eat(ch)) return "text does not match the format";
        } else {
            while (i + 1 < fmt.size() && (fmt[i + 1] == '-' || fmt[i + 1] == ':' || std::isdigit(static_cast<unsigned char>(fmt[i + 1])))) {
                ++i; // flags and widths are ignored when parsing
            }
            if (i + 1 >= fmt.size()) return "incomplete format";
            if (const char* err = parseToken(c, fmt[++i], p)) return err;
        }
    }
    return nullptr;
}

// YYYY-MM-DD[(T| )hh[:mm[:ss[.f]]][ ][Z|+hh[:mm]]], also the basic form (no dashes / colons).
const char* parseIso(Cursor& c, Parsed& p) {
    i64 y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (!c.digits(4, 4, y)) return "expected a 4-digit year";
    c.eat('-');
    if (!c.digits(2, 2, mo)) return "expected a month";
    c.eat('-');
    if (!c.digits(2, 2, d)) return "expected a day";
    p.year = int(y);
    p.month = int(mo);
    p.day = int(d);
    if (c.eof()) return nullptr;

    if (!(c.eat('T') || c.eat('t') || c.eat(' '))) return "expected 'T' after the date";
    if (!c.digits(2, 2, h)) return "expected an hour";
    const bool colon = c.eat(':');
    if (!c.digits(2, 2, mi)) return "expected minutes";
    p.hour = int(h);
    p.min = int(mi);
    if (colon ? c.eat(':') : c.atDigit()) {
        if (!c.digits(2, 2, s)) return "expected seconds";
        p.sec = int(s);
        if ((c.eat('.') || c.eat(',')) && !c.fraction(p.nsec)) return "expected fractional digits";
    }
    c.eat(' ');
    if (!c.eof() && !parseOffset(c, p)) return "bad UTC offset";
    return nullptr;
}

const char* validate(Parsed& p) {
    if (p.month < 1 || p.month > 12) return "month out of range";
    if (p.hour > 23 || p.min > 59 || p.sec > 60) return "time of day out of range";
    if (p.hour12 || p.pm >= 0) p.hour = p.hour % 12 + (p.pm == 1 ? 12 : 0);
    if (p.yday > 0) {
        if (p.yday > (isLeap(p.year) ? 366 : 365)) return "day of year out of range";
        p.month = 1;
        p.day = p.yday;
    } else if (p.day < 1 || p.day > daysInMonth(p.year, p.month)) {
        return "day out of range for the month";
    }
    return nullptr;
}

const char* parseText(std::string_view text, std::string_view fmt, Parsed& p) {
    const auto attempt = [&](auto&& parser) -> const char* {
        p = Parsed{};
        Cursor c{text.data(), text.data() + text.size()};
        if (const char* err = parser(c)) return err;
        return c.eof() ? nullptr : "unexpected trailing text";
    };

    const char* err;
    if (fmt == "iso") {
        err = attempt([&](Cursor& c) { return parseIso(c, p); });
        for (const char* rfc : {"%a, %d %b %Y %H:%M:%S %z", "%d %b %Y %H:%M:%S %z"}) {
            if (err) err = attempt([&](Cursor& c) { return parseInto(c, rfc, p); });
        }
        if (err) return "unrecognised date/time format";
    } else if ((err = attempt([&](Cursor& c) { return parseInto(c, presetOf(fmt), p); }))) {
        return err;
    }
    return validate(p);
}

Instant instantOf(const Parsed& p, const Zone& z) {
    if (p.hasEpoch) return {p.epoch, p.nsec};
    const i64 local = localSeconds(p.year, p.month, p.day, p.hour, p.min, p.sec);
    return {p.hasOffset ? local - p.offset : localToUtc(z, local), p.nsec};
}

// bindings ///////////////////////////////////////////////////////////

// datetime.now(): (number, number)
int dtNow(lua_State* L) {
    const i64 ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return pushInstant(L, {floorDiv(ns, kBillion), int(floorMod(ns, kBillion))});
}

// datetime.monotonic(): number
int dtMonotonic(lua_State* L) {
    lua_pushnumber(L, std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
    return 1;
}

// datetime.toFields(t: number, zone: (string | number)?, nsec: number?): table
int dtToFields(lua_State* L) {
    const Instant in = readInstant(L, 1, 3);
    const Zone z = readZone(L, 2);
    const Fields f = makeFields(z, in);

    const std::pair<const char*, double> numbers[] = {
        {"year", double(f.year)}, {"month", f.month}, {"day", f.day},         {"hour", f.hour},
        {"min", f.min},           {"sec", f.sec},     {"nsec", f.nsec},       {"wday", f.wday + 1},
        {"yday", f.yday},         {"isoWeek", f.isoWeek}, {"isoYear", double(f.isoYear)}, {"offset", f.offset},
    };

    lua_createtable(L, 0, 14);
    for (const auto& [key, value] : numbers) {
        lua_pushnumber(L, value);
        lua_setfield(L, -2, key);
    }
    lua_pushboolean(L, f.dst);
    lua_setfield(L, -2, "isdst");
    lua_pushstring(L, f.abbr);
    lua_setfield(L, -2, "abbr");
    return 1;
}

// datetime.fromFields(fields: table, zone: (string | number)?): (number, number)
int dtFromFields(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const Zone z = readZone(L, 2);

    const i64 year = i64(fieldNum(L, 1, "year", 1970));
    const i64 month = i64(fieldNum(L, 1, "month", 1));
    const i64 day = i64(fieldNum(L, 1, "day", 1));
    const i64 hour = i64(fieldNum(L, 1, "hour", 0));
    const i64 min = i64(fieldNum(L, 1, "min", 0));
    const i64 sec = i64(fieldNum(L, 1, "sec", 0));
    const i64 nsec = i64(fieldNum(L, 1, "nsec", 0));

    const i64 local = localSeconds(year, month, day, hour, min, sec);
    if (std::llabs(local) > kMaxSec * 2) luaL_error(L, "time out of range");
    return pushInstant(L, make(L, localToUtc(z, local), nsec));
}

// datetime.parse(text: string, format: string?, zone: (string | number)?): (number, number) | (nil, string)
int dtParse(lua_State* L) {
    std::size_t textLen = 0, fmtLen = 0;
    const char* text = luaL_checklstring(L, 1, &textLen);
    const char* fmt = luaL_optlstring(L, 2, "iso", &fmtLen);
    const Zone z = readZone(L, 3);

    Parsed p;
    if (const char* err = parseText(std::string_view(text, textLen), std::string_view(fmt, fmtLen), p)) {
        lua_pushnil(L);
        lua_pushstring(L, err);
        return 2;
    }
    const Instant in = instantOf(p, z);
    return pushInstant(L, make(L, in.sec, in.nsec));
}

// datetime.format(t: number, format: string?, zone: (string | number)?, nsec: number?): string
int dtFormat(lua_State* L) {
    std::size_t fmtLen = 0;
    const Instant in = readInstant(L, 1, 4);
    const char* fmt = luaL_optlstring(L, 2, "iso", &fmtLen);
    const Zone z = readZone(L, 3);

    const std::string_view name(fmt, fmtLen);
    const Fields f = makeFields(z, in);

    std::string out;
    formatInto(out, name == "iso" ? (f.offset == 0 ? "%FT%T%fZ" : "%FT%T%f%:z") : presetOf(name), f);
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

// datetime.add(t: number, delta: table, zone: (string | number)?, nsec: number?): (number, number)
int dtAdd(lua_State* L) {
    luaL_checktype(L, 2, LUA_TTABLE);
    const Instant in = readInstant(L, 1, 4);
    const Zone z = readZone(L, 3);

    const double years = fieldNum(L, 2, "years", 0), months = fieldNum(L, 2, "months", 0);
    const double weeks = fieldNum(L, 2, "weeks", 0), days = fieldNum(L, 2, "days", 0);
    const double hours = fieldNum(L, 2, "hours", 0, false), minutes = fieldNum(L, 2, "minutes", 0, false);
    const double seconds = fieldNum(L, 2, "seconds", 0, false), nsec = fieldNum(L, 2, "nsec", 0);

    i64 sec = in.sec;
    if (years != 0 || months != 0 || weeks != 0 || days != 0) { // calendar part: local wall clock
        const Fields f = makeFields(z, in);
        const i64 m0 = f.year * 12 + (f.month - 1) + i64(years) * 12 + i64(months);
        const i64 y = floorDiv(m0, 12);
        const int m = int(floorMod(m0, 12)) + 1;
        const int d = std::min(f.day, daysInMonth(y, m));
        const i64 date = daysFromCivil(y, m, d) + i64(weeks) * 7 + i64(days);
        sec = localToUtc(z, date * kDay + f.hour * 3600 + f.min * 60 + f.sec);
    }

    const double t = hours * 3600 + minutes * 60 + seconds; // exact-timeline part
    const double whole = std::floor(t);
    return pushInstant(L, make(L, sec + i64(whole), in.nsec + i64(nsec) + std::llround((t - whole) * double(kBillion))));
}

// datetime.startOf(t: number, unit: string, zone: (string | number)?, nsec: number?): (number, number)
int dtStartOf(lua_State* L) {
    const Instant in = readInstant(L, 1, 4);
    const std::string_view unit = luaL_checkstring(L, 2);
    const Zone z = readZone(L, 3);

    constexpr std::string_view kUnits[] = {"year", "month", "week", "day", "hour", "minute", "second"};
    const auto at = std::find(std::begin(kUnits), std::end(kUnits), unit);
    luaL_argcheck(L, at != std::end(kUnits), 2, "unit must be year, month, week, day, hour, minute or second");

    const int u = int(at - std::begin(kUnits));
    const Fields f = makeFields(z, in);
    const i64 day = u <= 1 ? 1 : f.day - (u == 2 ? (f.wday + 6) % 7 : 0); // week: back to Monday
    const i64 local = localSeconds(f.year, u <= 0 ? 1 : f.month, day, u <= 3 ? 0 : f.hour, u <= 4 ? 0 : f.min, u <= 5 ? 0 : f.sec);
    return pushInstant(L, make(L, localToUtc(z, local), 0));
}

// Numeric time scales: unix seconds = value * mul / div + offset (offset in unix seconds).
struct Scale {
    std::string_view name;
    double mul, div, offset;
};

constexpr Scale kScales[] = {
    {"unix", 1, 1, 0},
    {"unixms", 1, 1e3, 0},
    {"unixus", 1, 1e6, 0},
    {"julian", 86400, 1, -210866760000.0}, // days since -4712-01-01 12:00
    {"mjd", 86400, 1, -3506716800.0},      // days since 1858-11-17
    {"excel", 86400, 1, -2209161600.0},    // days since 1899-12-30 (1900 system, valid after 1900-03-01)
    {"filetime", 1, 1e7, -11644473600.0},  // 100 ns ticks since 1601-01-01
    {"dotnet", 1, 1e7, -62135596800.0},    // 100 ns ticks since 0001-01-01
    {"ntp", 1, 1, -2208988800.0},          // seconds since 1900-01-01
    {"cocoa", 1, 1, 978307200.0},          // seconds since 2001-01-01
};

const Scale& readScale(lua_State* L, int idx) {
    std::size_t len = 0;
    const char* s = luaL_checklstring(L, idx, &len);
    for (const Scale& scale : kScales) {
        if (scale.name == std::string_view(s, len)) return scale;
    }
    luaL_argerror(L, idx, "unknown time scale");
    return kScales[0];
}

// datetime.convert(value: number, from: string, to: string): number
int dtConvert(lua_State* L) {
    const double v = luaL_checknumber(L, 1);
    const Scale& from = readScale(L, 2);
    const Scale& to = readScale(L, 3);

    const double unix = v * from.mul / from.div + from.offset;
    lua_pushnumber(L, (unix - to.offset) * to.div / to.mul);
    return 1;
}

// datetime.isLeapYear(year: number): boolean
int dtIsLeapYear(lua_State* L) {
    lua_pushboolean(L, isLeap(luaL_checkinteger(L, 1)));
    return 1;
}

// datetime.daysInMonth(year: number, month: number): number
int dtDaysInMonth(lua_State* L) {
    const int year = luaL_checkinteger(L, 1);
    const int month = luaL_checkinteger(L, 2);
    luaL_argcheck(L, month >= 1 && month <= 12, 2, "month must be between 1 and 12");
    lua_pushnumber(L, daysInMonth(year, month));
    return 1;
}

constexpr NativeFunction kFunctions[] = {
    {"now", dtNow},
    {"monotonic", dtMonotonic},
    {"toFields", dtToFields},
    {"fromFields", dtFromFields},
    {"parse", dtParse},
    {"format", dtFormat},
    {"add", dtAdd},
    {"startOf", dtStartOf},
    {"convert", dtConvert},
    {"isLeapYear", dtIsLeapYear},
    {"daysInMonth", dtDaysInMonth},
};

} // namespace

void openDatetime(lua_State* L) {
    lua_createtable(L, 0, static_cast<int>(std::size(kFunctions)) + 1);
    setFunctions(L, kFunctions);
}

} // namespace sonata::lib::libs
