/*
 * util.c —— 基础工具（P1 真实实现）
 *
 * 随机数优先使用 Windows rand_s()（CSPRNG），不可用时回退 rand()。
 * 若目标平台没有 rand_s，编译时定义 UTIL_NO_RAND_S 即可强制走回退路径。
 */
#if defined(_WIN32) && !defined(UTIL_NO_RAND_S)
#  define _CRT_RAND_S 1
#  define UTIL_USE_RAND_S 1
#endif

#include "util.h"
#include "chat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

static const char UTIL_HEX[] = "0123456789abcdef";

/* ---- 随机数源 ---- */

/* 回退源：rand()，用 time + GetTickCount 播种一次 */
static void util_rand_fallback(unsigned char *buf, size_t len)
{
    static int seeded = 0;
    size_t i;

    if (!seeded) {
        unsigned int tick;
#if defined(_WIN32)
        tick = (unsigned int)GetTickCount();
#else
        tick = 0u;
#endif
        srand((unsigned int)time(NULL) ^ (tick << 16) ^ tick);
        seeded = 1;
    }
    for (i = 0; i < len; i++) {
        buf[i] = (unsigned char)(rand() & 0xFF);
    }
}

/* 填充 len 字节随机数据：优先 rand_s()，失败或不可用则回退 */
static void util_rand_bytes(unsigned char *buf, size_t len)
{
    size_t i = 0;

#if defined(UTIL_USE_RAND_S)
    while (i < len) {
        unsigned int v = 0;
        int j;

        if (rand_s(&v) != 0) {
            break; /* rand_s 失败，剩余字节交给回退源 */
        }
        for (j = 0; j < 4 && i < len; j++) {
            buf[i++] = (unsigned char)((v >> (j * 8)) & 0xFF);
        }
    }
#endif

    if (i < len) {
        util_rand_fallback(buf + i, len - i);
    }
}

void util_random_hex(int bytes, char *out)
{
    unsigned char chunk[32];
    int done = 0;

    if (out == NULL) {
        return;
    }
    if (bytes <= 0) {
        out[0] = '\0';
        return;
    }
    while (done < bytes) {
        int m = bytes - done;
        int i;

        if (m > (int)sizeof chunk) {
            m = (int)sizeof chunk;
        }
        util_rand_bytes(chunk, (size_t)m);
        for (i = 0; i < m; i++) {
            out[(done + i) * 2]     = UTIL_HEX[chunk[i] >> 4];
            out[(done + i) * 2 + 1] = UTIL_HEX[chunk[i] & 0x0F];
        }
        done += m;
    }
    out[bytes * 2] = '\0';
}

void util_gen_uid(char *out, size_t n)
{
    unsigned int v = 0;

    if (out == NULL || n == 0) {
        return;
    }
    util_rand_bytes((unsigned char *)&v, sizeof v);
    snprintf(out, n, "%08u", (unsigned)(v % 100000000u));
}

void util_gen_handle(char *out, size_t n)
{
    unsigned int v = 0;

    if (out == NULL || n == 0) {
        return;
    }
    util_rand_bytes((unsigned char *)&v, sizeof v);
    snprintf(out, n, "user_%04u", (unsigned)(v % 10000u));
}

void util_gen_guest_uid(char *out, size_t n)
{
    unsigned int v = 0;

    if (out == NULL || n == 0) {
        return;
    }
    util_rand_bytes((unsigned char *)&v, sizeof v);
    snprintf(out, n, "G-%04u", (unsigned)(v % 10000u));
}

void util_gen_token(char *out, size_t n)
{
    unsigned char buf[32];
    size_t limit;
    size_t i;

    if (out == NULL || n == 0) {
        return;
    }
    util_rand_bytes(buf, sizeof buf);

    limit = n - 1;
    if (limit > 64) {
        limit = 64;
    }
    for (i = 0; i < limit; i++) {
        unsigned char byte = buf[i / 2];

        out[i] = (i % 2 == 0) ? UTIL_HEX[byte >> 4] : UTIL_HEX[byte & 0x0F];
    }
    out[limit] = '\0';
}

int util_dm_room(int a, int b, char *out, size_t n)
{
    int lo = a < b ? a : b;
    int hi = a < b ? b : a;
    int need;

    if (out == NULL || n == 0) {
        return CHAT_ERR_FULL;
    }
    need = snprintf(out, n, "dm:%d:%d", lo, hi);
    if (need < 0 || (size_t)need >= n) {
        out[0] = '\0';
        return CHAT_ERR_FULL;
    }
    return CHAT_OK;
}

int util_parse_dm_room(const char *room, int *a, int *b)
{
    int x = 0;
    int y = 0;
    int consumed = 0;

    if (room == NULL || a == NULL || b == NULL) {
        return CHAT_ERR;
    }
    if (sscanf(room, "dm:%d:%d%n", &x, &y, &consumed) != 2) {
        return CHAT_ERR;
    }
    if (room[consumed] != '\0') {
        return CHAT_ERR;
    }
    *a = x;
    *b = y;
    return CHAT_OK;
}

static int util_hexval(int c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

int util_url_decode(const char *in, char *out, size_t n)
{
    size_t oi = 0;
    size_t i = 0;

    if (out == NULL || n == 0) {
        return 0;
    }
    if (in == NULL) {
        out[0] = '\0';
        return 0;
    }

    while (in[i] != '\0') {
        int ch;

        if (in[i] == '%' && util_hexval((unsigned char)in[i + 1]) >= 0 &&
            util_hexval((unsigned char)in[i + 2]) >= 0) {
            ch = (util_hexval((unsigned char)in[i + 1]) << 4) |
                 util_hexval((unsigned char)in[i + 2]);
            i += 3;
        } else if (in[i] == '+') {
            ch = ' ';
            i++;
        } else {
            ch = (unsigned char)in[i];
            i++;
        }

        if (oi + 1 >= n) {
            break; /* 缓冲区不足，截断 */
        }
        out[oi++] = (char)ch;
    }
    out[oi] = '\0';
    return (int)oi;
}

int util_valid_username(const char *s)
{
    size_t len;
    size_t i;

    if (s == NULL) {
        return 0;
    }
    len = strlen(s);
    if (len < 3 || len > MAX_USERNAME) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];

        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_')) {
            return 0;
        }
    }
    return 1;
}

int util_valid_handle(const char *s)
{
    size_t len;
    size_t i;

    if (s == NULL) {
        return 0;
    }
    len = strlen(s);
    if (len < 3 || len > MAX_HANDLE) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];

        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return 0;
        }
    }
    return 1;
}

int util_valid_text(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;

    if (s == NULL) {
        return 0;
    }

    while (*p != '\0') {
        if (*p < 0x20) {
            if (*p != '\t' && *p != '\n' && *p != '\r') {
                return 0;                /* 其它 C0 控制字符：入库会破坏 JSON 大小 */
            }
            p++;
            continue;
        }
        if (*p == 0x7F) {
            return 0;                    /* DEL */
        }
        if (*p < 0x80) {
            p++;
            continue;
        }

        /* 多字节 UTF-8：校验前导字节、续字节与过长编码 / 代理区 / U+10FFFF 上限 */
        {
            unsigned int cp;
            int extra;
            int i;

            if (*p >= 0xC2 && *p <= 0xDF)      { extra = 1; cp = *p & 0x1Fu; }
            else if (*p >= 0xE0 && *p <= 0xEF) { extra = 2; cp = *p & 0x0Fu; }
            else if (*p >= 0xF0 && *p <= 0xF4) { extra = 3; cp = *p & 0x07u; }
            else {
                return 0;                /* 非法前导字节（含 0x80-0xC1 与 0xF5-0xFF） */
            }

            for (i = 0; i < extra; i++) {
                unsigned char c = p[1 + i];

                if ((c & 0xC0) != 0x80) {
                    return 0;            /* 续字节格式错误或提前遇到 '\0' */
                }
                cp = (cp << 6) | (unsigned)(c & 0x3Fu);
            }

            if ((extra == 1 && cp < 0x80u) ||
                (extra == 2 && cp < 0x800u) ||
                (extra == 3 && cp < 0x10000u) ||
                (cp >= 0xD800u && cp <= 0xDFFFu) ||
                cp > 0x10FFFFu) {
                return 0;                /* 过长编码 / 代理区 / 超出范围 */
            }
            p += extra + 1;
        }
    }
    return 1;
}

void util_now_str(char *out, size_t n)
{
    time_t t;
    struct tm *lt;

    if (out == NULL || n == 0) {
        return;
    }
    t = time(NULL);
    lt = localtime(&t);
    if (lt == NULL) {
        out[0] = '\0';
        return;
    }
    if (strftime(out, n, "%Y-%m-%d %H:%M:%S", lt) == 0) {
        out[0] = '\0';
    }
}

long util_now_ts(void)
{
    return (long)time(NULL);
}
