/*
 * json.c —— JSON 输出辅助
 *
 * 阶段 0 即提供完整实现：本文件是无依赖的叶子工具，被数据层与路由层共同使用。
 */
#include "json.h"

#include <stdio.h>
#include <string.h>

static size_t escaped_len(const char *s)
{
    size_t n = 0;
    const unsigned char *p;

    if (s == NULL) {
        return 0;
    }

    for (p = (const unsigned char *)s; *p != '\0'; p++) {
        switch (*p) {
        case '"':
        case '\\':
        case '\n':
        case '\r':
        case '\t':
        case '\b':
        case '\f':
            n += 2;
            break;
        default:
            n += (*p < 0x20) ? 6 : 1;   /* 其它控制字符写成 \u00XX */
            break;
        }
    }
    return n;
}

static size_t escape_into(char *dst, const char *src)
{
    char *out = dst;
    const unsigned char *p;

    if (src == NULL) {
        return 0;
    }

    for (p = (const unsigned char *)src; *p != '\0'; p++) {
        switch (*p) {
        case '"':  *out++ = '\\'; *out++ = '"';  break;
        case '\\': *out++ = '\\'; *out++ = '\\'; break;
        case '\n': *out++ = '\\'; *out++ = 'n';  break;
        case '\r': *out++ = '\\'; *out++ = 'r';  break;
        case '\t': *out++ = '\\'; *out++ = 't';  break;
        case '\b': *out++ = '\\'; *out++ = 'b';  break;
        case '\f': *out++ = '\\'; *out++ = 'f';  break;
        default:
            if (*p < 0x20) {
                out += sprintf(out, "\\u%04x", (unsigned)*p);
            } else {
                *out++ = (char)*p;
            }
            break;
        }
    }
    *out = '\0';
    return (size_t)(out - dst);
}

void json_escape(const char *in, char *out, size_t n)
{
    size_t need;

    if (out == NULL || n == 0) {
        return;
    }

    need = escaped_len(in);
    if (need + 1 > n) {
        out[0] = '\0';
        return;
    }

    escape_into(out, in);
}

/* 在 out 末尾追加前缀：非空时先补一个逗号 */
static int append_sep(char *out, size_t n, size_t *len)
{
    size_t l = *len;

    if (l > 0) {
        if (l + 1 >= n) {
            return CHAT_ERR_FULL;
        }
        out[l++] = ',';
        out[l] = '\0';
    }
    *len = l;
    return CHAT_OK;
}

/* 判断在 out（当前已有 len 字节）之后追加
 *   [分隔逗号(0 或 1 字节)] + payload(payload_len 字节) + '\0'
 * 是否放得下。payload_len 必须是**实际要写入的字节数**，不能少算。 */
static int room_for(size_t len, size_t payload_len, size_t n)
{
    size_t sep = (len > 0) ? 1 : 0;

    return (len + sep + payload_len + 1 <= n) ? CHAT_OK : CHAT_ERR_FULL;
}

int json_append_str(char *out, size_t n, const char *key, const char *value)
{
    size_t len, key_len, esc_len, payload_len;

    if (out == NULL || n == 0 || key == NULL || value == NULL) {
        return CHAT_ERR;
    }

    len = strlen(out);
    key_len = strlen(key);
    esc_len = escaped_len(value);

    /* 要写入的内容："key":"value" -> 1 + key + 1 + 1 + 1 + value + 1 */
    payload_len = key_len + esc_len + 5;
    if (room_for(len, payload_len, n) != CHAT_OK) {
        return CHAT_ERR_FULL;
    }

    if (append_sep(out, n, &len) != CHAT_OK) {
        return CHAT_ERR_FULL;
    }

    out[len++] = '"';
    memcpy(out + len, key, key_len);
    len += key_len;
    out[len++] = '"';
    out[len++] = ':';
    out[len++] = '"';
    len += escape_into(out + len, value);
    out[len++] = '"';
    out[len] = '\0';
    return (int)payload_len;
}

int json_append_int(char *out, size_t n, const char *key, long value)
{
    size_t len, payload_len;
    char num[32];

    if (out == NULL || n == 0 || key == NULL) {
        return CHAT_ERR;
    }

    sprintf(num, "%ld", value);

    len = strlen(out);
    /* 要写入的内容："key":num -> 1 + key + 1 + 1 + num */
    payload_len = strlen(key) + strlen(num) + 3;
    if (room_for(len, payload_len, n) != CHAT_OK) {
        return CHAT_ERR_FULL;
    }

    if (append_sep(out, n, &len) != CHAT_OK) {
        return CHAT_ERR_FULL;
    }

    return sprintf(out + len, "\"%s\":%s", key, num);
}

int json_append_bool(char *out, size_t n, const char *key, int value)
{
    size_t len, payload_len;
    const char *lit = value ? "true" : "false";

    if (out == NULL || n == 0 || key == NULL) {
        return CHAT_ERR;
    }

    len = strlen(out);
    /* 要写入的内容："key":true 或 "key":false -> 1 + key + 1 + 1 + lit */
    payload_len = strlen(key) + strlen(lit) + 3;
    if (room_for(len, payload_len, n) != CHAT_OK) {
        return CHAT_ERR_FULL;
    }

    if (append_sep(out, n, &len) != CHAT_OK) {
        return CHAT_ERR_FULL;
    }

    return sprintf(out + len, "\"%s\":%s", key, lit);
}

int json_append_raw(char *out, size_t n, const char *raw)
{
    size_t len, payload_len;

    if (out == NULL || n == 0 || raw == NULL) {
        return CHAT_ERR;
    }

    len = strlen(out);
    payload_len = strlen(raw);

    if (room_for(len, payload_len, n) != CHAT_OK) {
        return CHAT_ERR_FULL;
    }

    if (append_sep(out, n, &len) != CHAT_OK) {
        return CHAT_ERR_FULL;
    }

    memcpy(out + len, raw, payload_len + 1);
    return (int)payload_len;
}

int json_array_push(char *out, size_t n, const char *raw)
{
    return json_append_raw(out, n, raw);
}
