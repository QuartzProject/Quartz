#include "support/Chars.hpp"

#if defined(_LIBCPP_VERSION) && (_LIBCPP_VERSION < 200000 || !_LIBCPP_AVAILABILITY_HAS_FROM_CHARS_FLOATING_POINT)
#include <cerrno>
#include <cstdlib>
#include <locale.h>
#include <string>
#if __has_include(<xlocale.h>)
#include <xlocale.h>
#endif
#define QUARTZ_PARSE_DOUBLE_FALLBACK 1
#endif

namespace quartz::support {

#ifndef QUARTZ_PARSE_DOUBLE_FALLBACK

std::from_chars_result parseDouble(const char* first, const char* last, double& value) {
    return std::from_chars(first, last, value);
}

#else

namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// The longest prefix from_chars would read: [-] then digits[.digits][e[+-]digits], inf, infinity or nan[(...)].
const char* scanGeneral(const char* p, const char* last) {
    if (p != last && *p == '-') {
        ++p;
    }
    auto word = [&](const char* w) {
        const char* q = p;
        for (; *w != '\0'; ++w, ++q) {
            if (q == last || (*q | 0x20) != *w) {
                return static_cast<const char*>(nullptr);
            }
        }
        return q;
    };
    if (const char* q = word("infinity")) {
        return q;
    }
    if (const char* q = word("inf")) {
        return q;
    }
    if (const char* q = word("nan")) {
        if (q != last && *q == '(') {
            const char* r = q + 1;
            while (r != last && (isDigit(*r) || ((*r | 0x20) >= 'a' && (*r | 0x20) <= 'z') || *r == '_')) {
                ++r;
            }
            if (r != last && *r == ')') {
                return r + 1;
            }
        }
        return q;
    }
    const char* digits = p;
    while (p != last && isDigit(*p)) {
        ++p;
    }
    bool any = p != digits;
    if (p != last && *p == '.') {
        const char* frac = ++p;
        while (p != last && isDigit(*p)) {
            ++p;
        }
        any = any || p != frac;
    }
    if (!any) {
        return nullptr;
    }
    if (p != last && (*p | 0x20) == 'e') {
        const char* q = p + 1;
        if (q != last && (*q == '+' || *q == '-')) {
            ++q;
        }
        const char* exp = q;
        while (q != last && isDigit(*q)) {
            ++q;
        }
        if (q != exp) {
            p = q;
        }
    }
    return p;
}

} // namespace

std::from_chars_result parseDouble(const char* first, const char* last, double& value) {
    const char* end = scanGeneral(first, last);
    if (end == nullptr) {
        return {first, std::errc::invalid_argument};
    }
    static const locale_t cLocale = newlocale(LC_ALL_MASK, "C", nullptr);
    std::string text(first, end);
    errno = 0;
    char* stop = nullptr;
    double d = strtod_l(text.c_str(), &stop, cLocale);
    // strtod also reports ERANGE for subnormal results, which from_chars returns as they are.
    if (errno == ERANGE && (d == 0 || d > 1.7976931348623157e308 || d < -1.7976931348623157e308)) {
        return {end, std::errc::result_out_of_range};
    }
    value = d;
    return {first + (stop - text.c_str()), std::errc{}};
}

#endif

} // namespace quartz::support
