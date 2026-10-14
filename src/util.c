#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <unistd.h>
#include <time.h>
#endif

/* dslab_tests compiles util.c without the -D; keep it linkable */
#ifndef DSLAB_RES_DIR_DEFAULT
#define DSLAB_RES_DIR_DEFAULT "."
#endif

uint64_t dslab_parse_duration_ns(const char *s, int *ok)
{
    *ok = 0;
    if (!s || !*s)
        return 0;
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s || v < 0)
        return 0;
    while (*end == ' ')
        end++;
    uint64_t mult = 1000000ULL; /* default ms */
    if (!*end) {
        /* bare number: milliseconds */
    } else if (!strcmp(end, "ns")) {
        mult = 1;
    } else if (!strcmp(end, "us") || !strcmp(end, "\xC2\xB5" "s")) {
        mult = 1000;
    } else if (!strcmp(end, "ms")) {
        mult = 1000000;
    } else if (!strcmp(end, "s")) {
        mult = 1000000000ULL;
    } else {
        return 0;
    }
    *ok = 1;
    return (uint64_t)(v * (double)mult + 0.5);
}

uint64_t dslab_parse_rate_hz(const char *s, int *ok)
{
    *ok = 0;
    if (!s || !*s)
        return 0;
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s || v <= 0)
        return 0;
    while (*end == ' ')
        end++;
    uint64_t mult = 1;
    char u = (char)toupper((unsigned char)*end); /* u is uppercase */
    if (!*end) {
        mult = 1;
    } else if (u == 'K' && !end[1]) {
        mult = 1000ULL;
    } else if (u == 'M' && !end[1]) {
        mult = 1000000ULL;
    } else if (u == 'G' && !end[1]) {
        mult = 1000000000ULL;
    } else if (u == 'H' && !strncasecmp(end, "hz", 2) && !end[2]) {
        mult = 1;
    } else if ((u == 'K' || u == 'M' || u == 'G') && !strncasecmp(end + 1, "hz", 2) && !end[3]) {
        mult = u == 'K' ? 1000ULL : u == 'M' ? 1000000ULL : 1000000000ULL;
    } else {
        return 0;
    }
    *ok = 1;
    return (uint64_t)(v * (double)mult + 0.5);
}

int dslab_parse_channels(const char *s, uint8_t idx[32])
{
    if (!s || !*s)
        return -1;
    int n = 0;
    const char *p = s;
    while (*p) {
        while (*p == ' ' || *p == ',')
            p++;
        if (!*p)
            break;
        if ((*p == 'd' || *p == 'D') && isdigit((unsigned char)p[1]))
            p++;
        if (!isdigit((unsigned char)*p))
            return -1;
        char *end = NULL;
        long v = strtol(p, &end, 10);
        if (end == p || v < 0 || v > 31)
            return -1;
        p = end;
        int dup = 0;
        for (int i = 0; i < n; i++)
            if (idx[i] == (uint8_t)v)
                dup = 1;
        if (!dup)
            idx[n++] = (uint8_t)v;
        if (n >= 32)
            return -1;
    }
    /* sort ascending */
    for (int i = 0; i < n - 1; i++)
        for (int j = i + 1; j < n; j++)
            if (idx[i] > idx[j]) {
                uint8_t t = idx[i];
                idx[i] = idx[j];
                idx[j] = t;
            }
    return n;
}

int dslab_mkdirs(const char *path)
{
    char tmp[1024];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp))
        return -1;
    memcpy(tmp, path, len + 1);
    /* strip trailing separators */
    while (len > 0 && (tmp[len - 1] == '/' || tmp[len - 1] == '\\'))
        tmp[--len] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = '\0';
#ifdef _WIN32
            if (_mkdir(tmp) != 0 && errno != EEXIST)
                return -1;
#else
            if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
                return -1;
#endif
            *p = c;
        }
    }
#ifdef _WIN32
    if (_mkdir(tmp) != 0 && errno != EEXIST)
        return -1;
#else
    if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
        return -1;
#endif
    return 0;
}

void dslab_now_iso8601(char buf[40])
{
#ifdef _WIN32
    SYSTEMTIME st;
    GetSystemTime(&st);
    _snprintf(buf, 40, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
              st.wMilliseconds);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    snprintf(buf, 40, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
#endif
}

uint64_t dslab_now_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

uint64_t dslab_unix_ms(void)
{
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    /* 100ns ticks since 1601-01-01 -> unix ms */
    return (u.QuadPart - 116444736000000000ULL) / 10000ULL;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

long dslab_getpid(void)
{
#ifdef _WIN32
    return (long)GetCurrentProcessId();
#else
    return (long)getpid();
#endif
}

void dslab_sleep_ms(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

void dslab_sample_time_str(uint64_t sample_idx, uint64_t rate_hz, char *buf, size_t bufsz)
{
    /* Exact integer decomposition: whole ns = sample_idx * 1e9 / rate */
    unsigned long long ns = (unsigned long long)(((unsigned __int128)sample_idx * 1000000000ULL) / rate_hz);
    snprintf(buf, bufsz, "%llu.%06llu ms", ns / 1000000ULL, (ns / 1000ULL) % 1000ULL);
}

const char *dslab_res_dir_resolve(void)
{
    static char buf[1024];
    const char *env = getenv("DSLAB_RES_DIR");
    if (env && *env)
        return env;

#ifdef _WIN32
    char exe[1024];
    if (GetModuleFileNameA(NULL, exe, sizeof(exe))) {
        char *slash = strrchr(exe, '\\');
        if (!slash)
            slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            snprintf(buf, sizeof(buf), "%s\\res", exe);
            struct stat st;
            if (stat(buf, &st) == 0 && (st.st_mode & S_IFDIR))
                return buf;
        }
    }
#else
    ssize_t nl = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (nl > 0) {
        buf[nl] = '\0';
        char *slash = strrchr(buf, '/');
        if (slash) {
            *slash = '\0';
            size_t off = strlen(buf);
            snprintf(buf + off, sizeof(buf) - off, "/res");
            struct stat st;
            if (stat(buf, &st) == 0 && S_ISDIR(st.st_mode))
                return buf;
        }
    }
#endif
    return DSLAB_RES_DIR_DEFAULT;
}

void dslab_format_bytes(uint64_t bytes, char *buf, size_t bufsz)
{
    const char *unit[] = { "B", "KiB", "MiB", "GiB" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 3) {
        v /= 1024.0;
        u++;
    }
    snprintf(buf, bufsz, u == 0 ? "%llu B" : "%.2f %s",
             u == 0 ? (unsigned long long)bytes : 0ULL,
             u == 0 ? "" : unit[u]);
}
