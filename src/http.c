/*
 * http.c —— HTTP 解析、响应与静态文件
 *
 * 包 P2（p2-httplayer）实现：请求行/头解析、请求体收取、
 * JSON/文本/静态文件响应、表单与查询参数、Cookie 读取。
 */
#include "http.h"
#include "util.h"
#include "sock.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/select.h>
#include <sys/time.h>
#endif

#define HTTP_HEADER_MAX   16384   /* 请求头允许的最大字节数 */
#define HTTP_RECV_TIMEOUT 5000    /* 单次 recv 超时（毫秒），防慢速连接挂死 */

/* 解析出的、本层需要的请求头字段 */
typedef struct {
    size_t clen;             /* Content-Length */
    int    has_clen;
    int    chunked;          /* Transfer-Encoding: chunked */
    int    expect_continue;  /* Expect: 100-continue */
} header_info_t;

/* ---------------- 基础辅助 ---------------- */

static int ci_equal(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int ci_contains(const char *hay, const char *needle)
{
    size_t nlen = strlen(needle);

    if (nlen == 0) {
        return 1;
    }
    for (; *hay != '\0'; hay++) {
        size_t i = 0;
        while (i < nlen && hay[i] != '\0' &&
               tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) {
            return 1;
        }
    }
    return 0;
}

/* 循环发送直到全部发完（TCP 可能只发出部分数据） */
static int send_all(int fd, const char *data, size_t len)
{
    size_t sent = 0;

    while (sent < len) {
        int n = (int)send(fd, data + sent, (int)(len - sent), 0);
        if (n <= 0) {
            return CHAT_ERR;
        }
        sent += (size_t)n;
    }
    return CHAT_OK;
}

static void http_set_recv_timeout(int fd)
{
#ifdef _WIN32
    DWORD tv = HTTP_RECV_TIMEOUT;
    setsockopt((SOCKET)fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec = HTTP_RECV_TIMEOUT / 1000;
    tv.tv_usec = (HTTP_RECV_TIMEOUT % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
#endif
}

/* 尽力把套接字上“已经到达”的数据读掉，避免 close 时因未读数据发 RST */
static void http_drain_socket(int fd)
{
    char tmp[1024];
    int guard = 0;

    while (guard++ < 64) {
        fd_set rfds;
        struct timeval tv;
        int r;

        FD_ZERO(&rfds);
#ifdef _WIN32
        FD_SET((SOCKET)fd, &rfds);
#else
        FD_SET(fd, &rfds);
#endif
        tv.tv_sec = 0;
        tv.tv_usec = 200000;   /* 最多多等 200ms，避免拖慢响应 */
        r = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (r <= 0) {
            break;
        }
        if (recv(fd, tmp, (int)sizeof(tmp), 0) <= 0) {
            break;
        }
    }
}

/* 丢弃尚未读到的请求体（have 为已读入的字节数） */
static void http_drain_body(int fd, size_t have, size_t clen)
{
    char tmp[2048];
    size_t remain = clen > have ? clen - have : 0;

    while (remain > 0) {
        size_t want = remain > sizeof(tmp) ? sizeof(tmp) : remain;
        int n = recv(fd, tmp, (int)want, 0);
        if (n <= 0) {
            break;
        }
        remain -= (size_t)n;
    }
}

/* ---------------- 请求解析 ---------------- */

static void http_parse_header_line(request_t *req, const char *line, header_info_t *info)
{
    const char *colon = strchr(line, ':');
    const char *value;
    char name[64];
    size_t nlen;

    if (colon == NULL) {
        return;
    }
    nlen = (size_t)(colon - line);
    while (nlen > 0 && (line[nlen - 1] == ' ' || line[nlen - 1] == '\t')) {
        nlen--;
    }
    if (nlen == 0 || nlen >= sizeof(name)) {
        return;
    }
    memcpy(name, line, nlen);
    name[nlen] = '\0';

    value = colon + 1;
    while (*value == ' ' || *value == '\t') {
        value++;
    }

    if (ci_equal(name, "Content-Length")) {
        char *endp = NULL;
        long v = strtol(value, &endp, 10);
        if (endp != value && v >= 0) {
            info->clen = (size_t)v;
            info->has_clen = 1;
        }
    } else if (ci_equal(name, "Transfer-Encoding")) {
        if (ci_contains(value, "chunked")) {
            info->chunked = 1;
        }
    } else if (ci_equal(name, "Expect")) {
        if (ci_contains(value, "100-continue")) {
            info->expect_continue = 1;
        }
    } else if (ci_equal(name, "Cookie")) {
        size_t vlen = strlen(value);
        if (vlen > MAX_COOKIE) {
            vlen = MAX_COOKIE;
        }
        memcpy(req->cookie, value, vlen);
        req->cookie[vlen] = '\0';
    }
}

/* 解析请求行 + 各行头；hdr 为可写的、以 '\0' 结尾的头部副本 */
static int http_parse_header(request_t *req, char *hdr, header_info_t *info)
{
    char *line_end;
    char *line;
    char *sp;
    size_t len;

    line_end = strstr(hdr, "\r\n");
    if (line_end == NULL) {
        return CHAT_ERR;
    }
    *line_end = '\0';

    /* 请求行：METHOD SP TARGET [SP VERSION] */
    sp = strchr(hdr, ' ');
    if (sp == NULL || sp == hdr) {
        return CHAT_ERR;
    }
    *sp = '\0';
    len = strlen(hdr);
    if (len >= sizeof(req->method)) {
        return CHAT_ERR;
    }
    memcpy(req->method, hdr, len + 1);

    line = sp + 1;
    sp = strchr(line, ' ');
    if (sp != NULL) {
        *sp = '\0';
    }

    /* 分离查询串 */
    sp = strchr(line, '?');
    if (sp != NULL) {
        *sp = '\0';
        sp++;
        len = strlen(sp);
        if (len > MAX_QUERY) {
            return CHAT_ERR;
        }
        memcpy(req->query, sp, len + 1);
    }

    /* 路径：空按 "/"；含 ".." 视为非法 */
    if (*line == '\0') {
        memcpy(req->path, "/", 2);
    } else {
        if (strstr(line, "..") != NULL) {
            return CHAT_ERR;
        }
        len = strlen(line);
        if (len > MAX_PATH) {
            return CHAT_ERR;
        }
        memcpy(req->path, line, len + 1);
    }

    /* 其余头部 */
    line = line_end + 2;
    while (*line != '\0') {
        char *next = strstr(line, "\r\n");
        if (next != NULL) {
            *next = '\0';
        }
        http_parse_header_line(req, line, info);
        if (next == NULL) {
            break;
        }
        line = next + 2;
    }
    return CHAT_OK;
}

/* 在 buf 中查找 "\r\n\r\n"，找到则把其起始下标写入 *pos */
static int http_find_header_end(const char *buf, size_t len, size_t *pos)
{
    size_t i;

    if (len < 4) {
        return 0;
    }
    for (i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            *pos = i;
            return 1;
        }
    }
    return 0;
}

int http_read_request(int fd, request_t *req)
{
    char raw[HTTP_HEADER_MAX + MAX_BODY_LEN + 8];
    header_info_t info;
    size_t total = 0;
    size_t hdr_len = 0;
    size_t body_start;
    size_t have;
    size_t got;
    int n;

    if (req == NULL) {
        return CHAT_ERR;
    }
    memset(req, 0, sizeof(*req));
    http_set_recv_timeout(fd);

    /* 1. 一直读到出现 "\r\n\r\n" */
    for (;;) {
        size_t pos;

        if (http_find_header_end(raw, total, &pos)) {
            hdr_len = pos;
            break;
        }
        if (total >= sizeof(raw)) {
            http_drain_socket(fd);
            return CHAT_ERR;
        }
        n = recv(fd, raw + total, (int)(sizeof(raw) - total), 0);
        if (n <= 0) {
            return CHAT_ERR;
        }
        total += (size_t)n;
    }

    if (hdr_len == 0 || hdr_len > HTTP_HEADER_MAX) {
        http_drain_socket(fd);
        return CHAT_ERR;
    }
    raw[hdr_len] = '\0';   /* 只会覆盖终止符的第一个 '\r'，不影响 body */

    memset(&info, 0, sizeof(info));
    if (http_parse_header(req, raw, &info) != CHAT_OK) {
        http_drain_socket(fd);
        return CHAT_ERR;
    }

    /* 2. 客户端声明 Expect: 100-continue 时先回 100，避免客户端等待后重发 */
    if (info.expect_continue) {
        (void)send_all(fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    }

    /* 3. chunked 无法预知长度：尽力读干净剩余数据，body 视为空 */
    if (info.chunked) {
        http_drain_socket(fd);
        return CHAT_OK;
    }

    /* 4. 按 Content-Length 把请求体读满（可能跨多次 recv） */
    body_start = hdr_len + 4;
    have = total > body_start ? total - body_start : 0;

    if (info.has_clen && info.clen > 0) {
        if (info.clen > MAX_BODY_LEN) {
            /* 超限：先把 body 读干净再报错，否则 close 会发 RST 丢掉响应 */
            http_drain_body(fd, have, info.clen);
            return CHAT_ERR;
        }
        if (have > info.clen) {
            have = info.clen;
        }
        if (have > 0) {
            memcpy(req->body, raw + body_start, have);
        }
        got = have;
        while (got < info.clen) {
            size_t want = info.clen - got;
            n = recv(fd, req->body + got, (int)want, 0);
            if (n <= 0) {
                return CHAT_ERR;
            }
            got += (size_t)n;
        }
        req->body[got] = '\0';
    }

    return CHAT_OK;
}

/* ---------------- 响应 ---------------- */

void http_respond_raw(int fd, int status, const char *content_type,
                      const char *data, size_t len)
{
    char header[1024];
    int header_len;

    header_len = snprintf(header, sizeof(header),
                          "HTTP/1.1 %d %s\r\n"
                          "Content-Type: %s\r\n"
                          "Content-Length: %lu\r\n"
                          "Connection: close\r\n"
                          "\r\n",
                          status, http_status_text(status),
                          content_type ? content_type : "application/octet-stream",
                          (unsigned long)len);
    if (header_len < 0) {
        return;
    }
    if ((size_t)header_len >= sizeof(header)) {
        return;
    }
    if (send_all(fd, header, (size_t)header_len) != CHAT_OK) {
        return;
    }
    if (data != NULL && len > 0) {
        (void)send_all(fd, data, len);
    }
}

void http_respond_json(int fd, int status, const char *json)
{
    const char *body = json ? json : "";

    http_respond_raw(fd, status, "application/json; charset=utf-8",
                     body, strlen(body));
}

void http_respond_json_cookie(int fd, int status, const char *json, const char *token)
{
    char header[1024];
    const char *body = json ? json : "";
    int header_len;

    if (token == NULL) {
        http_respond_json(fd, status, body);
        return;
    }

    header_len = snprintf(header, sizeof(header),
                          "HTTP/1.1 %d %s\r\n"
                          "Content-Type: application/json; charset=utf-8\r\n"
                          "Content-Length: %lu\r\n"
                          "Set-Cookie: sid=%s; Path=/; HttpOnly\r\n"
                          "Connection: close\r\n"
                          "\r\n",
                          status, http_status_text(status),
                          (unsigned long)strlen(body), token);
    if (header_len < 0 || (size_t)header_len >= sizeof(header)) {
        return;
    }
    if (send_all(fd, header, (size_t)header_len) != CHAT_OK) {
        return;
    }
    if (body[0] != '\0') {
        (void)send_all(fd, body, strlen(body));
    }
}

void http_respond_text(int fd, int status, const char *content_type, const char *body)
{
    const char *text = body ? body : "";

    http_respond_raw(fd, status,
                     content_type ? content_type : "text/plain; charset=utf-8",
                     text, strlen(text));
}

/* ---------------- 静态文件 ---------------- */

static const char *http_mime_type(const char *path)
{
    const char *ext = strrchr(path, '.');

    if (ext == NULL || strchr(ext, '/') != NULL || strchr(ext, '\\') != NULL) {
        return "application/octet-stream";
    }
    if (ci_equal(ext, ".html") || ci_equal(ext, ".htm")) return "text/html; charset=utf-8";
    if (ci_equal(ext, ".css"))  return "text/css; charset=utf-8";
    if (ci_equal(ext, ".js"))   return "application/javascript; charset=utf-8";
    if (ci_equal(ext, ".json")) return "application/json";
    if (ci_equal(ext, ".png"))  return "image/png";
    if (ci_equal(ext, ".svg"))  return "image/svg+xml";
    if (ci_equal(ext, ".ico"))  return "image/x-icon";
    return "application/octet-stream";
}

int http_serve_static(int fd, const char *path)
{
    const char *rel;
    char full[512];
    char header[1024];
    FILE *fp;
    char *buf;
    long fsize;
    int header_len;
    size_t len;

    if (path == NULL) {
        return CHAT_ERR;
    }

    /* 去掉 URL 的前导 '/'，其余一律当作相对于 CHAT_STATIC_ROOT 的路径 */
    rel = path;
    while (*rel == '/') {
        rel++;
    }
    if (*rel == '\0') {
        rel = "index.html";
    }
    /* 安全检查：禁止穿越、禁止 Windows 反斜杠与盘符绝对路径 */
    if (strstr(rel, "..") != NULL) {
        return CHAT_ERR;
    }
    if (strchr(rel, '\\') != NULL) {
        return CHAT_ERR;
    }
    if (isalpha((unsigned char)rel[0]) && rel[1] == ':') {
        return CHAT_ERR;
    }

    len = strlen(rel);
    if (len + sizeof(CHAT_STATIC_ROOT) + 1 >= sizeof(full)) {
        return CHAT_ERR;
    }
    snprintf(full, sizeof(full), CHAT_STATIC_ROOT "/%s", rel);

    fp = fopen(full, "rb");   /* 二进制模式，避免 Windows 文本模式改动字节 */
    if (fp == NULL) {
        return (errno == ENOENT) ? CHAT_ERR_NOTFOUND : CHAT_ERR;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return CHAT_ERR;
    }
    fsize = ftell(fp);
    if (fsize < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return CHAT_ERR;
    }

    buf = (char *)malloc((size_t)fsize + 1);
    if (buf == NULL) {
        fclose(fp);
        return CHAT_ERR;
    }
    if (fsize > 0 && fread(buf, 1, (size_t)fsize, fp) != (size_t)fsize) {
        free(buf);
        fclose(fp);
        return CHAT_ERR;
    }
    fclose(fp);

    header_len = snprintf(header, sizeof(header),
                          "HTTP/1.1 200 OK\r\n"
                          "Content-Type: %s\r\n"
                          "Content-Length: %lu\r\n"
                          "Cache-Control: no-cache\r\n"
                          "Connection: close\r\n"
                          "\r\n",
                          http_mime_type(rel), (unsigned long)fsize);
    if (header_len < 0 || (size_t)header_len >= sizeof(header)) {
        free(buf);
        return CHAT_ERR;
    }
    if (send_all(fd, header, (size_t)header_len) != CHAT_OK) {
        free(buf);
        return CHAT_ERR;
    }
    if (fsize > 0) {
        (void)send_all(fd, buf, (size_t)fsize);
    }
    free(buf);
    return CHAT_OK;
}

/* ---------------- 参数提取 ---------------- */

/* 在 x-www-form-urlencoded 串中取 key，值经 util_url_decode 解码后写入 out */
static int http_extract_param(const char *src, const char *key, char *out, size_t n)
{
    size_t klen;
    const char *p;

    if (out != NULL && n > 0) {
        out[0] = '\0';
    }
    if (src == NULL || key == NULL || key[0] == '\0' || out == NULL || n == 0) {
        return CHAT_ERR;
    }

    klen = strlen(key);
    p = src;
    while (*p != '\0') {
        const char *seg_end = p + strlen(p);
        const char *amp = strchr(p, '&');
        const char *eq;
        size_t name_len;

        if (amp != NULL) {
            seg_end = amp;
        }
        eq = (const char *)memchr(p, '=', (size_t)(seg_end - p));
        name_len = eq != NULL ? (size_t)(eq - p) : (size_t)(seg_end - p);

        if (name_len == klen && memcmp(p, key, klen) == 0) {
            const char *val = eq != NULL ? eq + 1 : seg_end;
            size_t val_len = (size_t)(seg_end - val);
            char tmp[MAX_BODY_LEN + 1];

            if (val_len > MAX_BODY_LEN) {
                val_len = MAX_BODY_LEN;   /* 超长截断 */
            }
            memcpy(tmp, val, val_len);
            tmp[val_len] = '\0';
            (void)util_url_decode(tmp, out, n);
            out[n - 1] = '\0';            /* 保证以 '\0' 结尾且不溢出 */
            return CHAT_OK;
        }
        if (amp == NULL) {
            break;
        }
        p = amp + 1;
    }
    return CHAT_ERR_NOTFOUND;
}

int http_param(const request_t *req, const char *key, char *out, size_t n)
{
    if (req == NULL) {
        if (out != NULL && n > 0) {
            out[0] = '\0';
        }
        return CHAT_ERR_NOTFOUND;
    }
    return http_extract_param(req->body, key, out, n);
}

int http_query_param(const request_t *req, const char *key, char *out, size_t n)
{
    if (req == NULL) {
        if (out != NULL && n > 0) {
            out[0] = '\0';
        }
        return CHAT_ERR_NOTFOUND;
    }
    return http_extract_param(req->query, key, out, n);
}

int http_cookie_sid(const request_t *req, char *out, size_t n)
{
    const char *p;

    if (out != NULL && n > 0) {
        out[0] = '\0';
    }
    if (req == NULL || out == NULL || n == 0) {
        return CHAT_ERR;
    }

    p = req->cookie;
    while (*p != '\0') {
        const char *end;
        size_t vlen;

        while (*p == ' ' || *p == ';' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        end = strchr(p, ';');
        if (end == NULL) {
            end = p + strlen(p);
        }
        if ((size_t)(end - p) >= 4 && strncmp(p, "sid=", 4) == 0) {
            const char *val = p + 4;
            vlen = (size_t)(end - val);
            while (vlen > 0 && (val[vlen - 1] == ' ' || val[vlen - 1] == '\t')) {
                vlen--;
            }
            if (vlen >= n) {
                vlen = n - 1;
            }
            memcpy(out, val, vlen);
            out[vlen] = '\0';
            return CHAT_OK;
        }
        p = end;
    }
    return CHAT_ERR_NOTFOUND;
}

const char *http_status_text(int status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 415: return "Unsupported Media Type";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default:  return "Unknown";
    }
}
