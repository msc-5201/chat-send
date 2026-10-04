/*
 * auth.c —— 鉴权与账号路由（真实实现，包 P4）
 *
 * 覆盖：注册 / 登录 / 游客 / 登出 / 我 / 改身份码 / 会话解析。
 * 响应结构与状态码遵循 API.md；所有响应由本文件自行发送。
 */
#include "auth.h"

#include "db.h"
#include "http.h"
#include "json.h"
#include "sha256.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* salt(32) + password(<=64) 的拼接缓冲，取值保证不截断 */
#define HASH_INPUT_MAX 192

/* ---------------------------------------------------------------- 会话辅助 */

int auth_from_request(const request_t *req, session_t *out)
{
    char token[MAX_TOKEN + 1];
    session_t s;

    if (out == NULL) {
        return CHAT_ERR;
    }
    memset(out, 0, sizeof(*out));

    if (http_cookie_sid(req, token, sizeof(token)) != CHAT_OK || token[0] == '\0') {
        return CHAT_ERR_NOTFOUND;
    }
    if (db_session_get(token, &s) != CHAT_OK) {
        return CHAT_ERR_NOTFOUND;
    }

    /* 非游客：sessions 表里的 handle/username 可能是注册时的旧值，
     * 用户改过身份码后必须实时从 users 表刷新，否则 /api/me 与加好友会用到过期身份码。 */
    if (!s.is_guest) {
        user_t u;

        if (db_user_by_id(s.user_id, &u) == CHAT_OK) {
            memcpy(s.uid, u.uid, sizeof(s.uid));
            memcpy(s.handle, u.handle, sizeof(s.handle));
            memcpy(s.username, u.username, sizeof(s.username));
        }
    }

    db_session_touch(token);
    memcpy(s.token, token, sizeof(s.token));
    *out = s;
    return CHAT_OK;
}

/* ---------------------------------------------------------------- 公共小工具 */

/* 统一的失败响应：{"ok":false,"error":"<中文文案>"} */
static void send_error(int fd, int status, const char *msg)
{
    char esc[256];
    char body[320];

    json_escape(msg, esc, sizeof(esc));
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", esc);
    http_respond_json(fd, status, body);
}

/* 拼身份 JSON：{ok,id,mode,uid,handle,username[,is_guest]}，字符串由 json_append_str 负责转义 */
static int build_identity(char *out, size_t n, int id, int is_guest,
                          const char *uid, const char *handle, const char *username,
                          int with_is_guest)
{
    char inner[1024];
    int written;

    inner[0] = '\0';
    if (json_append_bool(inner, sizeof(inner), "ok", 1) < 0 ||
        json_append_int(inner, sizeof(inner), "id", id) < 0 ||
        json_append_str(inner, sizeof(inner), "mode", is_guest ? "guest" : "normal") < 0 ||
        json_append_str(inner, sizeof(inner), "uid", uid) < 0 ||
        json_append_str(inner, sizeof(inner), "handle", handle) < 0 ||
        json_append_str(inner, sizeof(inner), "username", username) < 0) {
        return CHAT_ERR_FULL;
    }
    if (with_is_guest && json_append_bool(inner, sizeof(inner), "is_guest", is_guest) < 0) {
        return CHAT_ERR_FULL;
    }

    written = snprintf(out, n, "{%s}", inner);
    if (written < 0 || (size_t)written >= n) {
        return CHAT_ERR_FULL;
    }
    return CHAT_OK;
}

/* 登录/注册/游客成功后统一下发 Cookie + 身份 JSON；token 为 NULL 时不下发 Cookie */
static void send_identity(int fd, int id, int is_guest, const char *uid,
                          const char *handle, const char *username,
                          const char *token, int with_is_guest)
{
    char json[1200];

    if (build_identity(json, sizeof(json), id, is_guest, uid, handle, username,
                       with_is_guest) != CHAT_OK) {
        send_error(fd, 500, "服务器内部错误");
        return;
    }
    http_respond_json_cookie(fd, 200, json, token);
}

/* pass_hash = SHA256(salt 直接拼接 password)，不截断 */
static int make_pass_hash(const char *salt, const char *password, char *out)
{
    char buf[HASH_INPUT_MAX];
    int need = snprintf(buf, sizeof(buf), "%s%s", salt, password);

    if (need < 0 || (size_t)need >= sizeof(buf)) {
        return CHAT_ERR_FULL;
    }
    sha256_hex(buf, (size_t)need, out);
    return CHAT_OK;
}

/* 常数时间比较：逐字节 XOR 累加，不做提前返回（长度不同直接判否） */
static int ct_equal(const char *a, const char *b)
{
    size_t la, lb, i;
    unsigned int diff = 0;

    if (a == NULL || b == NULL) {
        return 0;
    }
    la = strlen(a);
    lb = strlen(b);
    if (la != lb) {
        return 0;
    }
    for (i = 0; i < la; i++) {
        diff |= (unsigned int)((unsigned char)a[i] ^ (unsigned char)b[i]);
    }
    return diff == 0;
}

/* ---------------------------------------------------------------- 游客 id */

/* 游客 user_id 用不断递减的负数（-1、-2、-3…）：
 * 每个游客在 messages.sender_id 上唯一，且与真实用户（正数）永不冲突。
 * 多线程下必须原子递减，否则会重复分配。 */
static long g_guest_seq = 0;

static int next_guest_id(void)
{
#ifdef _WIN32
    return (int)InterlockedDecrement((volatile LONG *)&g_guest_seq);
#else
    return (int)__sync_fetch_and_sub(&g_guest_seq, 1) - 1;
#endif
}

/* ---------------------------------------------------------------- 注册 */

int auth_register(const request_t *req, int fd)
{
    /* 取参缓冲故意大于字段上限：http_param 会按缓冲截断，
     * 用小缓冲会把 33 位用户名静默截成 32 位而误判合法。 */
    char username[128];
    char password[128];
    char salt[33];
    char hash[65];
    char uid[MAX_UID + 1];
    char handle[MAX_HANDLE + 1];
    char token[MAX_TOKEN + 1];
    user_t u;
    int i;
    int rc;

    if (http_param(req, "username", username, sizeof(username)) != CHAT_OK ||
        http_param(req, "password", password, sizeof(password)) != CHAT_OK ||
        username[0] == '\0' || password[0] == '\0') {
        send_error(fd, 400, "请输入用户名和密码");
        return CHAT_ERR;
    }
    if (!util_valid_username(username)) {
        send_error(fd, 400, "用户名需为 3-32 位字母、数字或下划线");
        return CHAT_ERR;
    }
    if (strlen(password) < 6 || strlen(password) > 64) {
        send_error(fd, 400, "密码长度需为 6-64 位");
        return CHAT_ERR;
    }

    util_random_hex(16, salt);
    if (make_pass_hash(salt, password, hash) != CHAT_OK) {
        send_error(fd, 500, "服务器内部错误");
        return CHAT_ERR;
    }

    /* uid 冲突重试（最多 20 次） */
    uid[0] = '\0';
    for (i = 0; i < 20; i++) {
        user_t probe;

        util_gen_uid(uid, sizeof(uid));
        if (db_user_by_uid(uid, &probe) != CHAT_OK) {
            break;
        }
    }
    if (i == 20) {
        send_error(fd, 500, "系统繁忙，请稍后重试");
        return CHAT_ERR;
    }

    /* handle 冲突重试 */
    handle[0] = '\0';
    for (i = 0; i < 20; i++) {
        util_gen_handle(handle, sizeof(handle));
        if (!db_user_handle_taken(handle, -1)) {
            break;
        }
    }
    if (i == 20) {
        send_error(fd, 500, "系统繁忙，请稍后重试");
        return CHAT_ERR;
    }

    memset(&u, 0, sizeof(u));
    rc = db_user_create(username, hash, salt, uid, handle, &u);
    if (rc == CHAT_ERR_TAKEN) {
        send_error(fd, 409, "用户名已存在");
        return CHAT_ERR;
    }
    if (rc != CHAT_OK) {
        send_error(fd, 500, "服务器内部错误");
        return CHAT_ERR;
    }

    util_gen_token(token, sizeof(token));
    if (db_session_create(token, u.id, 0, u.uid, u.handle, u.username) != CHAT_OK) {
        send_error(fd, 500, "服务器内部错误");
        return CHAT_ERR;
    }

    send_identity(fd, u.id, 0, u.uid, u.handle, u.username, token, 0);
    return CHAT_OK;
}

/* ---------------------------------------------------------------- 登录 */

int auth_login(const request_t *req, int fd)
{
    char username[128];
    char password[128];
    char salt[65];
    char stored[65];
    char calc[65];
    char token[MAX_TOKEN + 1];
    user_t u;
    int rc;

    if (http_param(req, "username", username, sizeof(username)) != CHAT_OK ||
        http_param(req, "password", password, sizeof(password)) != CHAT_OK ||
        username[0] == '\0' || password[0] == '\0') {
        send_error(fd, 400, "请输入用户名和密码");
        return CHAT_ERR;
    }

    memset(&u, 0, sizeof(u));
    rc = db_user_by_username(username, &u, stored, sizeof(stored), salt, sizeof(salt));
    if (rc == CHAT_ERR_NOTFOUND) {
        /* 不区分「用户不存在」与「密码错误」，避免用户名枚举 */
        send_error(fd, 401, "用户名或密码错误");
        return CHAT_ERR;
    }
    if (rc != CHAT_OK) {
        send_error(fd, 500, "服务器内部错误");
        return CHAT_ERR;
    }

    if (make_pass_hash(salt, password, calc) != CHAT_OK || !ct_equal(calc, stored)) {
        send_error(fd, 401, "用户名或密码错误");
        return CHAT_ERR;
    }

    util_gen_token(token, sizeof(token));
    if (db_session_create(token, u.id, 0, u.uid, u.handle, u.username) != CHAT_OK) {
        send_error(fd, 500, "服务器内部错误");
        return CHAT_ERR;
    }

    send_identity(fd, u.id, 0, u.uid, u.handle, u.username, token, 0);
    return CHAT_OK;
}

/* ---------------------------------------------------------------- 游客 */

int auth_guest(const request_t *req, int fd)
{
    char uid[MAX_UID + 1];
    char username[MAX_USERNAME + 1];
    char token[MAX_TOKEN + 1];
    const char *tail;
    size_t len;
    int guest_id;

    (void)req;

    util_gen_guest_uid(uid, sizeof(uid));

    /* 用户名取 uid 后 4 位：G-4821 -> 游客4821 */
    len = strlen(uid);
    tail = (len > 4) ? uid + len - 4 : uid;
    snprintf(username, sizeof(username), "游客%s", tail);

    guest_id = next_guest_id();

    util_gen_token(token, sizeof(token));
    if (db_session_create(token, guest_id, 1, uid, "", username) != CHAT_OK) {
        send_error(fd, 500, "服务器内部错误");
        return CHAT_ERR;
    }

    send_identity(fd, guest_id, 1, uid, "", username, token, 1);
    return CHAT_OK;
}

/* ---------------------------------------------------------------- 登出 */

int auth_logout(const request_t *req, int fd)
{
    char token[MAX_TOKEN + 1];

    if (http_cookie_sid(req, token, sizeof(token)) == CHAT_OK && token[0] != '\0') {
        db_session_delete(token);
    }
    /* 传空串让 P2 发出 Set-Cookie: sid=; ...，覆盖客户端上已失效的会话 Cookie。
     * 即使没有会话也返回 200，登出保持幂等。 */
    http_respond_json_cookie(fd, 200, "{\"ok\":true}", "");
    return CHAT_OK;
}

/* ---------------------------------------------------------------- 我 */

int auth_me(const request_t *req, int fd)
{
    session_t s;
    char json[1200];

    if (auth_from_request(req, &s) != CHAT_OK) {
        send_error(fd, 401, "未登录");
        return CHAT_ERR;
    }

    if (build_identity(json, sizeof(json), s.user_id, s.is_guest,
                       s.uid, s.handle, s.username, 1) != CHAT_OK) {
        send_error(fd, 500, "服务器内部错误");
        return CHAT_ERR;
    }
    http_respond_json(fd, 200, json);
    return CHAT_OK;
}

/* ---------------------------------------------------------------- 修改身份码 */

int auth_set_handle(const request_t *req, int fd)
{
    session_t s;
    char handle[128];   /* 大于字段上限，避免超长身份码被截断后误判合法 */
    char inner[256];
    char json[320];
    int written;
    int rc;

    if (auth_from_request(req, &s) != CHAT_OK) {
        send_error(fd, 401, "未登录");
        return CHAT_ERR;
    }
    if (s.is_guest) {
        send_error(fd, 403, "游客不能修改身份码");
        return CHAT_ERR;
    }
    if (http_param(req, "handle", handle, sizeof(handle)) != CHAT_OK ||
        handle[0] == '\0' || !util_valid_handle(handle)) {
        send_error(fd, 400, "身份码需为 3-32 位字母、数字、下划线或中划线");
        return CHAT_ERR;
    }
    if (db_user_handle_taken(handle, s.user_id)) {
        send_error(fd, 409, "该身份码已被占用");
        return CHAT_ERR;
    }

    rc = db_user_set_handle(s.user_id, handle);
    if (rc == CHAT_ERR_TAKEN) {          /* 并发下被别人抢先占用 */
        send_error(fd, 409, "该身份码已被占用");
        return CHAT_ERR;
    }
    if (rc != CHAT_OK) {
        send_error(fd, 500, "服务器内部错误");
        return CHAT_ERR;
    }

    inner[0] = '\0';
    if (json_append_bool(inner, sizeof(inner), "ok", 1) < 0 ||
        json_append_str(inner, sizeof(inner), "handle", handle) < 0) {
        send_error(fd, 500, "服务器内部错误");
        return CHAT_ERR;
    }
    written = snprintf(json, sizeof(json), "{%s}", inner);
    if (written < 0 || (size_t)written >= sizeof(json)) {
        send_error(fd, 500, "服务器内部错误");
        return CHAT_ERR;
    }
    http_respond_json(fd, 200, json);
    return CHAT_OK;
}
