/*
 * transfer.c —— 点对点文件传输信令路由（真实实现）
 *
 * 职责：在互为好友的两个用户之间转发 WebRTC 信令。
 *   - POST /api/transfer/send：把一条信令投递到「对方」的信箱
 *   - GET  /api/transfer/poll：取出并清空「本人」信箱里的信令
 *
 * 鉴权：必须登录且非游客；接收方必须是发送方的好友（db_friend_are）。
 * 信令内容（SDP / ICE candidate）以字符串原样透传，服务器不解析、不校验其语义。
 *
 * 信箱是进程内存态（无持久化），因此需要独立互斥锁保护——
 * 服务器是「每连接一线程」，锁的实现与 db.c 保持一致（Windows 临界区 / POSIX 互斥量）。
 */
#include "transfer.h"

#include "auth.h"
#include "db.h"
#include "http.h"
#include "json.h"
#include "notify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 信箱里允许保留的最大信令条数（超限时丢弃最旧的一条，防止内存无限增长） */
#define MAX_PENDING_SIGNALS 512

/* 一条待投递的信令 */
typedef struct {
    int  from;
    int  to;
    long id;
    char kind[8];              /* offer / answer / ice */
    char *payload;             /* 堆分配，随信令消费释放 */
} signal_node;

static signal_node *g_signals = NULL;
static size_t       g_count   = 0;
static size_t       g_cap     = 0;
static long         g_next_id = 1;

/* ---- 锁（与 db.c 相同套路：Windows 懒初始化临界区，POSIX 静态互斥量） ---- */

#ifdef _WIN32
#include <windows.h>
static CRITICAL_SECTION g_lock;
static volatile LONG    g_lock_state = 0;

static void lock_ensure(void)
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

static void mailbox_lock(void)
{
    lock_ensure();
    EnterCriticalSection(&g_lock);
}

static void mailbox_unlock(void)
{
    LeaveCriticalSection(&g_lock);
}
#else
#include <pthread.h>
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void mailbox_lock(void)
{
    pthread_mutex_lock(&g_lock);
}

static void mailbox_unlock(void)
{
    pthread_mutex_unlock(&g_lock);
}
#endif

/* ------------------------------------------------------------ 小工具 */

/* 统一错误响应（msg 均为本文件内部常量，无需转义） */
static void respond_error(int fd, int status, const char *msg)
{
    char buf[256];

    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", msg);
    http_respond_json(fd, status, buf);
}

/* 严格解析正整数 id，非法返回 0 */
static int parse_id(const char *s)
{
    long v;
    char *end;

    if (s == NULL || s[0] == '\0') {
        return 0;
    }
    v = strtol(s, &end, 10);
    if (*end != '\0' || v <= 0 || v > 2147483647L) {
        return 0;
    }
    return (int)v;
}

/* 把一条信令投递到 to 的信箱。成功返回 CHAT_OK */
static int mailbox_push(int from, int to, const char *kind, const char *payload)
{
    signal_node s;
    size_t plen = strlen(payload);

    s.payload = (char *)malloc(plen + 1);
    if (s.payload == NULL) {
        return CHAT_ERR;
    }
    memcpy(s.payload, payload, plen + 1);
    s.from = from;
    s.to   = to;
    {
        size_t klen = strlen(kind);
        if (klen >= sizeof(s.kind)) {
            klen = sizeof(s.kind) - 1;
        }
        memcpy(s.kind, kind, klen);
        s.kind[klen] = '\0';
    }

    mailbox_lock();
    /* g_next_id 是多线程共享的计数器，必须在信箱锁内递增，否则存在数据竞争 */
    s.id = g_next_id++;
    if (g_count >= MAX_PENDING_SIGNALS) {
        /* 丢掉最旧的一条，为新信令腾位置 */
        free(g_signals[0].payload);
        memmove(&g_signals[0], &g_signals[1],
                (g_count - 1) * sizeof(*g_signals));
        g_count--;
    }
    if (g_count == g_cap) {
        size_t nc = g_cap ? g_cap * 2 : 16;
        signal_node *na = (signal_node *)realloc(g_signals, nc * sizeof(*g_signals));
        if (na == NULL) {
            mailbox_unlock();
            free(s.payload);
            return CHAT_ERR;
        }
        g_signals = na;
        g_cap = nc;
    }
    g_signals[g_count++] = s;
    mailbox_unlock();
    notify_ping();               /* 唤醒对方挂在 /api/transfer/poll 上的长轮询 */
    return CHAT_OK;
}

/* 把一条信令拼成 {"from":N,"kind":"...","payload":"..."} 追加进 out 数组。
 * payload 经 json_append_str 转义。空间不足返回 CHAT_ERR_FULL */
static int push_signal_item(char *out, size_t n, const signal_node *s)
{
    size_t cap = strlen(s->payload) * 6 + 160;
    char *item = (char *)malloc(cap);
    char *obj;
    int ok = 0;

    if (item == NULL) {
        return CHAT_ERR;
    }
    item[0] = '\0';
    if (json_append_int(item, cap, "from", s->from) >= 0 &&
        json_append_str(item, cap, "kind", s->kind) >= 0 &&
        json_append_str(item, cap, "payload", s->payload) >= 0) {
        ok = 1;
    }

    if (ok) {
        obj = (char *)malloc(strlen(item) + 3);
        if (obj != NULL) {
            sprintf(obj, "{%s}", item);
            json_array_push(out, n, obj);
            free(obj);
        }
    }
    free(item);
    return ok ? CHAT_OK : CHAT_ERR;
}

/* ------------------------------------------------------------ 路由实现 */

/* POST /api/transfer/send */
int transfer_send(const request_t *req, int fd)
{
    session_t me;
    char to_buf[16];
    char kind[16];
    char payload[MAX_BODY_LEN + 1];
    int to;

    if (auth_from_request(req, &me) != CHAT_OK) {
        respond_error(fd, 401, "未登录");
        return CHAT_ERR;
    }
    if (me.is_guest) {
        respond_error(fd, 403, "游客不能使用文件传输");
        return CHAT_ERR;
    }

    if (http_param(req, "to", to_buf, sizeof(to_buf)) != CHAT_OK ||
        (to = parse_id(to_buf)) <= 0) {
        respond_error(fd, 400, "参数不合法");
        return CHAT_ERR;
    }
    if (http_param(req, "kind", kind, sizeof(kind)) != CHAT_OK ||
        (strcmp(kind, "offer") != 0 &&
         strcmp(kind, "answer") != 0 &&
         strcmp(kind, "ice") != 0)) {
        respond_error(fd, 400, "参数不合法");
        return CHAT_ERR;
    }
    if (http_param(req, "payload", payload, sizeof(payload)) != CHAT_OK ||
        payload[0] == '\0') {
        respond_error(fd, 400, "参数不合法");
        return CHAT_ERR;
    }
    if (to == me.user_id) {
        respond_error(fd, 400, "不能发送给自己");
        return CHAT_ERR;
    }
    if (db_friend_are(me.user_id, to) != CHAT_OK) {
        respond_error(fd, 403, "你们还不是好友");
        return CHAT_ERR;
    }

    if (mailbox_push(me.user_id, to, kind, payload) != CHAT_OK) {
        respond_error(fd, 500, "服务器内部错误");
        return CHAT_ERR;
    }

    http_respond_json(fd, 200, "{\"ok\":true}");
    return CHAT_OK;
}

/* 估算属于 uid 的信令序列化后需要的缓冲大小（payload 转义最多放大约 6 倍） */
static size_t signals_need(int uid)
{
    size_t i, need = 64;

    mailbox_lock();
    for (i = 0; i < g_count; i++) {
        if (g_signals[i].to == uid) {
            need += strlen(g_signals[i].payload) * 6 + 160;
        }
    }
    mailbox_unlock();
    return need;
}

/* 收集并消费（删除）属于 uid 的信令，拼成 JSON 数组片段写入 out */
static void collect_signals(int uid, char *out, size_t n)
{
    size_t i, w;

    mailbox_lock();
    out[0] = '\0';
    for (i = 0; i < g_count; i++) {
        if (g_signals[i].to == uid) {
            push_signal_item(out, n, &g_signals[i]);
        }
    }
    /* 消费（删除）属于本人的信令 */
    w = 0;
    for (i = 0; i < g_count; i++) {
        if (g_signals[i].to != uid) {
            g_signals[w++] = g_signals[i];
        } else {
            free(g_signals[i].payload);
        }
    }
    g_count = w;
    mailbox_unlock();
}

/* GET /api/transfer/poll */
int transfer_poll(const request_t *req, int fd)
{
    session_t me;
    size_t need;
    char *out;
    char *resp;
    long v0;
    int rc;

    if (auth_from_request(req, &me) != CHAT_OK) {
        respond_error(fd, 401, "未登录");
        return CHAT_ERR;
    }
    if (me.is_guest) {
        respond_error(fd, 403, "游客不能使用文件传输");
        return CHAT_ERR;
    }

    v0 = notify_ver();
    need = signals_need(me.user_id);
    out = (char *)malloc(need);
    if (out == NULL) {
        respond_error(fd, 500, "服务器内部错误");
        return CHAT_ERR;
    }
    collect_signals(me.user_id, out, need);

    /* 长轮询：信箱为空时阻塞等待新信令，而不是让前端每秒空轮询。
     * 等待期间可能涌入大量新信令，缓冲可能不够，按最新估算扩容一次。 */
    if (out[0] == '\0') {
        size_t need2;

        notify_wait_until(v0, CHAT_POLL_WAIT_MS);
        need2 = signals_need(me.user_id);
        if (need2 > need) {
            char *out2 = (char *)realloc(out, need2);

            if (out2 == NULL) {
                free(out);
                respond_error(fd, 500, "服务器内部错误");
                return CHAT_ERR;
            }
            out = out2;
            need = need2;
        }
        collect_signals(me.user_id, out, need);
    }

    rc = CHAT_OK;
    resp = (char *)malloc(strlen(out) + 64);
    if (resp == NULL) {
        free(out);
        respond_error(fd, 500, "服务器内部错误");
        return CHAT_ERR;
    }
    snprintf(resp, strlen(out) + 64, "{\"ok\":true,\"signals\":[%s]}", out);
    http_respond_json(fd, 200, resp);
    free(resp);
    free(out);
    return rc;
}
