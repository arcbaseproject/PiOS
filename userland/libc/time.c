// time.h: no RTC, so the clock starts at the epoch on every boot; UTC only.
#include "ulib.h"
#include "time.h"

time_t time(time_t *t)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (t)
        *t = ts.tv_sec;
    return ts.tv_sec;
}

clock_t clock(void)     // shortcut: wall time since boot, not CPU time
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * CLOCKS_PER_SEC + ts.tv_nsec / 1000;
}

double difftime(time_t a, time_t b) { return (double)(a - b); }

// Days from 1970-01-01 to y-m-d (proleptic Gregorian), m 1..12.
static long days_from_civil(long y, int m, int d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}



time_t mktime(struct tm *tm)
{
    long y = tm->tm_year + 1900L + tm->tm_mon / 12;
    int m = tm->tm_mon % 12;
    if (m < 0)
        m += 12, y--;
    long days = days_from_civil(y, m + 1, 1) + tm->tm_mday - 1;
    time_t t = days * 86400 + tm->tm_hour * 3600L + tm->tm_min * 60L + tm->tm_sec;
    *tm = *gmtime(&t);          // normalize fields like the real one
    return t;
}

struct tm *gmtime(const time_t *t)
{
    static struct tm tm;
    long s = *t, days = s / 86400, rem = s % 86400;
    if (rem < 0)
        rem += 86400, days--;
    tm.tm_hour = (int)(rem / 3600);
    tm.tm_min = (int)(rem / 60 % 60);
    tm.tm_sec = (int)(rem % 60);
    tm.tm_wday = (int)(((days + 4) % 7 + 7) % 7);      // 1970-01-01 was a Thursday
    long z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    int d = (int)(doy - (153 * mp + 2) / 5 + 1), m = (int)(mp < 10 ? mp + 3 : mp - 9);
    long y = yoe + era * 400 + (m <= 2);
    tm.tm_year = (int)(y - 1900);
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    tm.tm_yday = (int)(days - days_from_civil(y, 1, 1));
    tm.tm_isdst = 0;

    return &tm;
}

struct tm *localtime(const time_t *t) { return gmtime(t); }

static const char *const wday[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday",
                                    "Friday", "Saturday" };
static const char *const mon[] = { "January", "February", "March", "April", "May", "June", "July",
                                   "August", "September", "October", "November", "December" };

size_t strftime(char *s, size_t max, const char *f, const struct tm *tm)
{
    size_t n = 0;
    char tmp[64];
    for (; *f; f++) {
        const char *add = tmp;
        if (*f != '%') {
            tmp[0] = *f, tmp[1] = 0;
        } else {
            switch (*++f) {
            case 'a': snprintf(tmp, sizeof tmp, "%.3s", wday[tm->tm_wday % 7]); break;
            case 'A': add = wday[tm->tm_wday % 7]; break;
            case 'b': case 'h': snprintf(tmp, sizeof tmp, "%.3s", mon[tm->tm_mon % 12]); break;
            case 'B': add = mon[tm->tm_mon % 12]; break;
            case 'c': snprintf(tmp, sizeof tmp, "%.3s %.3s %2d %02d:%02d:%02d %d",
                               wday[tm->tm_wday % 7], mon[tm->tm_mon % 12], tm->tm_mday,
                               tm->tm_hour, tm->tm_min, tm->tm_sec, tm->tm_year + 1900); break;
            case 'd': snprintf(tmp, sizeof tmp, "%02d", tm->tm_mday); break;
            case 'H': snprintf(tmp, sizeof tmp, "%02d", tm->tm_hour); break;
            case 'I': snprintf(tmp, sizeof tmp, "%02d", (tm->tm_hour + 11) % 12 + 1); break;
            case 'j': snprintf(tmp, sizeof tmp, "%03d", tm->tm_yday + 1); break;
            case 'm': snprintf(tmp, sizeof tmp, "%02d", tm->tm_mon + 1); break;
            case 'M': snprintf(tmp, sizeof tmp, "%02d", tm->tm_min); break;
            case 'p': add = tm->tm_hour < 12 ? "AM" : "PM"; break;
            case 'S': snprintf(tmp, sizeof tmp, "%02d", tm->tm_sec); break;
            case 'U': snprintf(tmp, sizeof tmp, "%02d", (tm->tm_yday + 7 - tm->tm_wday) / 7); break;
            case 'W': snprintf(tmp, sizeof tmp, "%02d", (tm->tm_yday + 7 - (tm->tm_wday + 6) % 7) / 7); break;
            case 'w': snprintf(tmp, sizeof tmp, "%d", tm->tm_wday); break;
            case 'x': snprintf(tmp, sizeof tmp, "%02d/%02d/%02d", tm->tm_mon + 1, tm->tm_mday, tm->tm_year % 100); break;
            case 'X': snprintf(tmp, sizeof tmp, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec); break;
            case 'y': snprintf(tmp, sizeof tmp, "%02d", tm->tm_year % 100); break;
            case 'Y': snprintf(tmp, sizeof tmp, "%d", tm->tm_year + 1900); break;
            case 'Z': add = "UTC"; break;
            case '%': add = "%"; break;
            default: return 0;
            }
        }
        size_t k = strlen(add);
        if (n + k >= max)
            return 0;
        memcpy(s + n, add, k);
        n += k;
    }
    s[n] = 0;
    return n;
}
