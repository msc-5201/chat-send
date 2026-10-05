/*
 * dashboard.c —— 用户仪表盘：个人数据落盘 + GET /api/dashboard
 *
 * 文件布局（相对 server.exe 所在目录，main.c 启动时会切工作目录）：
 *     Dashboard/<用户名>/<用户名>.json
 *
 * 安全设计：
 *   - 目录名 / 文件名直接由用户名拼出，因此写盘前**再强制校验一次**用户名只含
 *     [A-Za-z0-9_]，杜绝 ".."、'/'、'\'、':' 造成目录穿越或写到他处；
 *   - 文件里只写非敏感个人数据，绝不写 pass_hash / salt / 会话 token；
 *   - 先写同目录 .tmp 再 rename，中途失败不会留下半截 JSON；
 *   - 服务器是「每连接一线程」，本文件用一把独立互斥锁串行化落盘，
 *     避免两个线程同时写同一个用户的文件。
 */
#include "dashboard.h"

#include "auth.h"
#include "db.h"
#include "http.h"
#include "json.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>          /* _mkdir */
#else
#include <sys/stat.h>        /* mkdir */
#include <sys/types.h>
#endif

/* 个人数据 JSON 上限：字段都很短，4KB 绰绰有余 */
#define DASH_JSON_MAX 4096
/* 路径缓冲：根目录 + 用户名(<=32) + 文件名(<=37) + 分隔符 + 余量 */
#define DASH_PATH_MAX 256

/* ------------------------------------------------------------ 独立互斥锁 */

#ifdef _WIN32
#include <windows.h>
static CRITICAL_SECTION g_lock;
static volatile LONG    g_lock_state = 0;

static void dash_lock(void)
{
    for (;;) {
        LONG st = InterlockedCompareExchange(&g_lock_state, 1, 0);
        if (st == 0) {
            InitializeCriticalSection(&g_lock);
            InterlockedExchange(&g_lock_state, 2);
            return;
        }
        if (st == 2) {
            return;
        }
        Sleep(0);
    }
}

static void dash_unlock(void)
{
    LeaveCriticalSection(&g_lock);
}
#else
#include <pthread.h>
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void dash_lock(void)   { pthread_mutex_lock(&g_lock); }
static void dash_unlock(void) { pthread_mutex_unlock(&g_lock); }
#endif

/* ------------------------------------------------------------ 小工具 */

/* 统一错误响应（msg 均为本文件内部常量，无需转义） */
static void send_error(int fd, int status, const char *msg)
{
    char buf[256];

    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", msg);
    http_respond_json(fd, status, buf);
}

/*
 * 用户名是否可以直接当目录名 / 文件名用。
 * 与 util_valid_username() 同规则，但这里**必须**独立再校验一次：
 * 落盘路径由它决定，任何 '.'、'/'、'\'、':' 都可能造成目录穿越。
 */
static int name_is_safe(const char *name)
{
    size_t i, len;

    if (name == NULL) {
        return 0;
    }
    len = strlen(name);
    if (len < 1 || len > MAX_USERNAME) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];

        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_')) {
            return 0;
        }
    }
    return 1;
}

/* 建目录；已存在视为成功 */
static int mkdir_one(const char *path)
{
#ifdef _WIN32
    if (_mkdir(path) == 0) {
        return CHAT_OK;
    }
#else
    if (mkdir(path, 0755) == 0) {
        return CHAT_OK;
    }
#endif
    return (errno == EEXIST) ? CHAT_OK : CHAT_ERR;
}

static void fmt_time(long ts, char *out, size_t n)
{
    time_t t = (time_t)ts;
    struct tm *lt = localtime(&t);

    if (lt == NULL || strftime(out, n, "%Y-%m-%d %H:%M:%S", lt) == 0) {
        if (n > 0) {
            out[0] = '\0';
        }
    }
}

/* 先写 .tmp 再改名，避免中途失败留下半截 JSON */
static int write_file_atomic(const char *path, const char *data, size_t len)
{
    char tmp[DASH_PATH_MAX];
    FILE *fp;
    size_t wrote;
    int closed;
    int written;

    written = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (written < 0 || (size_t)written >= sizeof(tmp)) {
        return CHAT_ERR;
    }
    fp = fopen(tmp, "wb");
    if (fp == NULL) {
        return CHAT_ERR;
    }
    /* 先落盘再关闭，各自判断一次：fclose 无论成败都只能调用一次 */
    wrote = fwrite(data, 1, len, fp);
    closed = fclose(fp);
    if (wrote != len || closed != 0) {
        remove(tmp);
        return CHAT_ERR;
    }
    remove(path);                    /* Windows 的 rename 不覆盖已存在文件 */
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return CHAT_ERR;
    }
    return CHAT_OK;
}

/* 拼个人数据对象的内容（不含首尾花括号） */
static int build_user_json(const user_t *u, long created_at, int friend_count,
                           int message_count, char *out, size_t n)
{
    char created[32];
    char updated[32];

    fmt_time(created_at, created, sizeof(created));
    fmt_time((long)time(NULL), updated, sizeof(updated));

    out[0] = '\0';
    if (json_append_int(out, n, "schema", 1) < 0 ||
        json_append_str(out, n, "username", u->username) < 0 ||
        json_append_int(out, n, "id", u->id) < 0 ||
        json_append_str(out, n, "uid", u->uid) < 0 ||
        json_append_str(out, n, "handle", u->handle) < 0 ||
        json_append_bool(out, n, "is_guest", 0) < 0 ||
        json_append_str(out, n, "created_at", created) < 0 ||
        json_append_int(out, n, "created_at_unix", created_at) < 0 ||
        json_append_int(out, n, "friend_count", friend_count) < 0 ||
        json_append_int(out, n, "message_count", message_count) < 0 ||
        json_append_str(out, n, "updated_at", updated) < 0) {
        return CHAT_ERR_FULL;
    }
    return CHAT_OK;
}

/* 组装 <根目录>/<用户名>/<用户名>.json 两个路径；成功返回 CHAT_OK */
static int build_paths(const char *username, char *dir, size_t dir_n,
                       char *path, size_t path_n)
{
    int w;

    w = snprintf(dir, dir_n, CHAT_DASHBOARD_ROOT "/%s", username);
    if (w < 0 || (size_t)w >= dir_n) {
        return CHAT_ERR_FULL;
    }
    w = snprintf(path, path_n, "%s/%s.json", dir, username);
    if (w < 0 || (size_t)w >= path_n) {
        return CHAT_ERR_FULL;
    }
    return CHAT_OK;
}

/* ------------------------------------------------------------ 落盘 */

int dashboard_write_user(const user_t *u)
{
    char inner[DASH_JSON_MAX];
    char json[DASH_JSON_MAX + 8];
    char dir[DASH_PATH_MAX];
    char path[DASH_PATH_MAX];
    long created_at = 0;
    int friends = 0;
    int msgs = 0;
    int written;
    int rc;

    if (u == NULL || !name_is_safe(u->username)) {
        return CHAT_ERR;
    }
    /* 统计在 dash_lock 之外取（内部用 db_lock，两把锁互不嵌套） */
    if (db_user_stats(u->id, &created_at, &friends, &msgs) != CHAT_OK) {
        return CHAT_ERR;
    }
    if (build_paths(u->username, dir, sizeof(dir), path, sizeof(path)) != CHAT_OK) {
        return CHAT_ERR_FULL;
    }

    dash_lock();
    if (mkdir_one(CHAT_DASHBOARD_ROOT) != CHAT_OK || mkdir_one(dir) != CHAT_OK) {
        dash_unlock();
        return CHAT_ERR;
    }
    if (build_user_json(u, created_at, friends, msgs, inner, sizeof(inner)) != CHAT_OK) {
        dash_unlock();
        return CHAT_ERR_FULL;
    }
    written = snprintf(json, sizeof(json), "{%s}\n", inner);
    if (written < 0 || (size_t)written >= sizeof(json)) {
        dash_unlock();
        return CHAT_ERR_FULL;
    }
    rc = write_file_atomic(path, json, (size_t)written);
    if (rc != CHAT_OK) {
        fprintf(stderr, "[dashboard] 写入失败: %s (errno=%d)\n", path, errno);
    }
    dash_unlock();
    return rc;
}

/* ------------------------------------------------------------ 路由 */

/* GET /api/dashboard */
int dashboard_get(const request_t *req, int fd)
{
    session_t s;
    user_t u;
    char inner[DASH_JSON_MAX];
    char head[512];
    char json[DASH_JSON_MAX + 1024];
    char dir[DASH_PATH_MAX];
    char path[DASH_PATH_MAX];
    long created_at = 0;
    int friends = 0;
    int msgs = 0;
    int file_ok = 0;
    int written;

    if (auth_from_request(req, &s) != CHAT_OK) {
        send_error(fd, 401, "未登录");
        return CHAT_ERR;
    }
    if (s.is_guest) {
        send_error(fd, 403, "游客没有仪表盘，请注册后使用");
        return CHAT_ERR;
    }
    if (db_user_by_id(s.user_id, &u) != CHAT_OK) {
        send_error(fd, 500, "服务器内部错误");
        return CHAT_ERR;
    }
    if (db_user_stats(s.user_id, &created_at, &friends, &msgs) != CHAT_OK) {
        send_error(fd, 500, "服务器内部错误");
        return CHAT_ERR;
    }
    if (!name_is_safe(u.username) ||
        build_paths(u.username, dir, sizeof(dir), path, sizeof(path)) != CHAT_OK) {
        send_error(fd, 500, "服务器内部错误");
        return CHAT_ERR;
    }

    /* 同一份内容：既写文件（自愈），也作为响应体返回 */
    dash_lock();
    if (build_user_json(&u, created_at, friends, msgs, inner, sizeof(inner)) == CHAT_OK &&
        mkdir_one(CHAT_DASHBOARD_ROOT) == CHAT_OK && mkdir_one(dir) == CHAT_OK) {
        written = snprintf(json, sizeof(json), "{%s}\n", inner);
        if (written > 0 && (size_t)written < sizeof(json) &&
            write_file_atomic(path, json, (size_t)written) == CHAT_OK) {
            file_ok = 1;
        }
    }
    dash_unlock();

    if (inner[0] == '\0') {
        send_error(fd, 500, "服务器内部错误");
        return CHAT_ERR;
    }

    head[0] = '\0';
    if (json_append_bool(head, sizeof(head), "ok", 1) < 0 ||
        json_append_str(head, sizeof(head), "path", path) < 0 ||
        json_append_bool(head, sizeof(head), "file_written", file_ok) < 0) {
        send_error(fd, 500, "服务器内部错误");
        return CHAT_ERR;
    }
    written = snprintf(json, sizeof(json), "{%s,\"dashboard\":{%s}}", head, inner);
    if (written < 0 || (size_t)written >= sizeof(json)) {
        send_error(fd, 500, "服务器内部错误");
        return CHAT_ERR;
    }
    http_respond_json(fd, 200, json);
    return CHAT_OK;
}
