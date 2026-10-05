/*
 * message.c —— 消息路由（真实实现，包 P6）
 *
 * 覆盖：世界聊天 / 私聊的消息读取与发送、轮询（当前房间新消息 + 好友申请）。
 * 房间规则见 normalize_room()：只允许 "#world" 与 "dm:<小id>:<大id>"；
 * 私聊必须由当前会话本人参与、双方互为好友，游客不允许私聊。
 * 响应状态码与 JSON 结构逐条对照 API.md「3. 消息」。
 *
 * 所有缓冲区都在栈上或堆上就地分配，不使用静态/全局缓冲，
 * 因为服务器是「每连接一线程」。
 */
#include "message.h"

#include "auth.h"
#include "db.h"
#include "http.h"
#include "notify.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* db_msg_fetch_json / db_friend_requests_json 的输出缓冲（堆分配，避免占用线程栈） */
#define MSG_BUF_SIZE (256 * 1024)
#define REQ_BUF_SIZE (64 * 1024)

/* ------------------------------------------------------------ 小工具 */

/* 安全写入定长缓冲：超长时截断，始终以 '\0' 结尾 */
static void put_str(char *dst, size_t n, const char *src)
{
    size_t len;

    if (dst == NULL || n == 0) {
        return;
    }
    if (src == NULL) {
        src = "";
    }
    len = strlen(src);
    if (len >= n) {
        len = n - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* 把 src 拷到 dst 并返回新的写入位置（调用者保证缓冲足够大） */
static char *append_str(char *dst, const char *src)
{
    size_t len = strlen(src);

    memcpy(dst, src, len);
    return dst + len;
}

/* 全空白（含空串）返回 1 */
static int is_blank(const char *s)
{
    size_t i;

    for (i = 0; s[i] != '\0'; i++) {
        if (s[i] != ' ' && s[i] != '\t' && s[i] != '\r' && s[i] != '\n') {
            return 0;
        }
    }
    return 1;
}

/* 解析查询参数 after：缺省、非法、越界（含负值）一律按 0 处理 */
static long parse_after_id(const request_t *req)
{
    char buf[32];
    char *end = NULL;
    long v;

    if (http_query_param(req, "after", buf, sizeof buf) != CHAT_OK) {
        return 0;
    }
    errno = 0;
    v = strtol(buf, &end, 10);
    if (end == buf || *end != '\0' || errno == ERANGE || v < 0) {
        return 0;
    }
    return v;
}

/* ------------------------------------------------------------ 房间校验 */

/*
 * 校验并规范化房间名。
 *
 * 合法形式只有两种：
 *   1. "#world"（任何人，含游客，均可读写）
 *   2. "dm:<a>:<b>"：必须是「当前会话本人参与的私聊」且双方互为好友，游客一律拒绝。
 *      解析成功后用 util_dm_room() 按数值升序重排，保证 dm:9:3 与 dm:3:9 命中同一房间。
 *
 * 成功返回 CHAT_OK，并把规范化后的房间名写入 out；
 * 失败返回要响应的 HTTP 状态码（400 格式非法 / 403 越权），错误文案写入 err。
 */
static int normalize_room(const char *in, const session_t *me,
                          char *out, size_t n, char *err, size_t err_n)
{
    int a = 0;
    int b = 0;
    int other;

    if (in == NULL || in[0] == '\0') {
        put_str(err, err_n, "房间名不合法");
        return 400;
    }
    if (strcmp(in, ROOM_WORLD) == 0) {
        put_str(out, n, ROOM_WORLD);
        return CHAT_OK;
    }
    if (util_parse_dm_room(in, &a, &b) != CHAT_OK) {
        put_str(err, err_n, "房间名不合法");
        return 400;
    }
    /* 私聊房间名格式合法，之后一律按越权处理 */
    if (me->is_guest) {
        put_str(err, err_n, "游客不能使用私聊");
        return 403;
    }
    if (a == me->user_id) {
        other = b;
    } else if (b == me->user_id) {
        other = a;
    } else {
        other = 0;                 /* 双方都不是自己：不能读写别人的私聊 */
    }
    if (other <= 0 || other == me->user_id) {
        put_str(err, err_n, "无权访问该私聊房间");
        return 403;
    }
    if (util_dm_room(a, b, out, n) != CHAT_OK) {
        put_str(err, err_n, "房间名不合法");
        return 400;
    }
    if (db_friend_are(me->user_id, other) != CHAT_OK) {
        put_str(err, err_n, "你们还不是好友，无法私聊");
        return 403;
    }
    return CHAT_OK;
}

/* ------------------------------------------------------------ JSON 输出 */

/*
 * 取消息 JSON 片段中最后一条消息的 id。
 * 片段形如 {"id":1,...},{"id":2,...}：反复 strstr 找最后一次出现的 "\"id\":" 再 strtol。
 * 片段为空（没有新消息）时返回 fallback（即本次请求的 after_id）。
 * 注："sender_id" 不含子串 "\"id\":"，消息正文中的引号已被 json_escape 转义，
 *     因此不会误匹配。
 */
static long last_id_of(const char *json_fragment, long fallback)
{
    const char *p;
    long last = fallback;

    if (json_fragment == NULL || json_fragment[0] == '\0') {
        return fallback;
    }
    p = json_fragment;
    while ((p = strstr(p, "\"id\":")) != NULL) {
        p += 5;                    /* 跳过 "\"id\":" */
        last = strtol(p, NULL, 10);
    }
    return last;
}

/* 统一的错误响应 {"ok":false,"error":"<msg>"}（msg 均为本文件内部常量，无需转义） */
static void respond_error(int fd, int status, const char *msg)
{
    static const char *const head = "{\"ok\":false,\"error\":\"";
    static const char *const tail = "\"}";
    size_t len = strlen(head) + strlen(msg) + strlen(tail);
    char *out = (char *)malloc(len + 1);
    char *p;

    if (out == NULL) {
        http_respond_json(fd, 500, "{\"ok\":false,\"error\":\"服务器内部错误\"}");
        return;
    }
    p = append_str(out, head);
    p = append_str(p, msg);
    p = append_str(p, tail);
    *p = '\0';
    http_respond_json(fd, status, out);
    free(out);
}

/*
 * 手工拼装 {"ok":true,"room":"<room>","messages":[<frag>]}。
 * 刻意不使用 json_append_raw/json_array_push：json_append_* 只要缓冲非空就会补逗号，
 * 拼方括号时极易产出非法 JSON。frag 为空串时输出 []。
 * room 只会是 "#world" 或 "dm:<数字>:<数字>"，无需转义。
 */
static void respond_messages(int fd, const char *room, const char *frag)
{
    static const char *const head = "{\"ok\":true,\"room\":\"";
    static const char *const mid = "\",\"messages\":[";
    static const char *const tail = "]}";
    size_t len = strlen(head) + strlen(room) + strlen(mid) + strlen(frag) + strlen(tail);
    char *out = (char *)malloc(len + 1);
    char *p;

    if (out == NULL) {
        http_respond_json(fd, 500, "{\"ok\":false,\"error\":\"服务器内部错误\"}");
        return;
    }
    p = append_str(out, head);
    p = append_str(p, room);
    p = append_str(p, mid);
    p = append_str(p, frag);
    p = append_str(p, tail);
    *p = '\0';
    http_respond_json(fd, 200, out);
    free(out);
}

/* 手工拼装 {"ok":true,"messages":[<msgs>],"requests":[<reqs>],"last_id":<id>,"friends_rev":<rev>} */
static void respond_poll(int fd, const char *msgs, const char *reqs,
                         long last_id, long friends_rev)
{
    static const char *const head = "{\"ok\":true,\"messages\":[";
    static const char *const mid = "],\"requests\":[";
    char idbuf[96];
    size_t len;
    char *out;
    char *p;

    snprintf(idbuf, sizeof idbuf,
             "],\"last_id\":%ld,\"friends_rev\":%ld}", last_id, friends_rev);
    len = strlen(head) + strlen(msgs) + strlen(mid) + strlen(reqs) + strlen(idbuf);
    out = (char *)malloc(len + 1);
    if (out == NULL) {
        http_respond_json(fd, 500, "{\"ok\":false,\"error\":\"服务器内部错误\"}");
        return;
    }
    p = append_str(out, head);
    p = append_str(p, msgs);
    p = append_str(p, mid);
    p = append_str(p, reqs);
    p = append_str(p, idbuf);
    *p = '\0';
    http_respond_json(fd, 200, out);
    free(out);
}

/* ------------------------------------------------------------ 路由实现 */

/* GET /api/messages */
int message_fetch(const request_t *req, int fd)
{
    session_t me;
    char raw[MAX_ROOM + 1];
    char room[MAX_ROOM + 1];
    char err[64];
    char *msgbuf;
    long after_id;
    int rc;

    if (auth_from_request(req, &me) != CHAT_OK) {
        respond_error(fd, 401, "未登录");
        return CHAT_ERR;
    }
    if (http_query_param(req, "room", raw, sizeof raw) != CHAT_OK) {
        put_str(raw, sizeof raw, ROOM_WORLD);   /* 缺省世界聊天 */
    }
    after_id = parse_after_id(req);

    rc = normalize_room(raw, &me, room, sizeof room, err, sizeof err);
    if (rc != CHAT_OK) {
        respond_error(fd, rc, err);
        return CHAT_ERR;
    }

    msgbuf = (char *)malloc(MSG_BUF_SIZE);
    if (msgbuf == NULL) {
        respond_error(fd, 500, "服务器内部错误");
        return CHAT_ERR;
    }
    rc = db_msg_fetch_json(room, after_id, msgbuf, MSG_BUF_SIZE);
    if (rc == CHAT_ERR_FULL) {
        free(msgbuf);
        respond_error(fd, 500, "消息过多，请刷新重试");
        return CHAT_ERR;
    }
    if (rc != CHAT_OK) {
        free(msgbuf);
        respond_error(fd, 500, "服务器内部错误");
        return CHAT_ERR;
    }

    respond_messages(fd, room, msgbuf);
    free(msgbuf);
    return CHAT_OK;
}

/* POST /api/messages */
int message_send(const request_t *req, int fd)
{
    session_t me;
    char raw[MAX_ROOM + 1];
    char room[MAX_ROOM + 1];
    char err[64];
    /* 用 MAX_BODY_LEN 级缓冲接收，才能分辨「正好 2000」与「超过 2000」 */
    char body[MAX_BODY_LEN + 1];
    char resp[64];
    int id;
    int rc;

    if (auth_from_request(req, &me) != CHAT_OK) {
        respond_error(fd, 401, "未登录");
        return CHAT_ERR;
    }
    if (http_param(req, "room", raw, sizeof raw) != CHAT_OK) {
        put_str(raw, sizeof raw, ROOM_WORLD);
    }
    if (http_param(req, "body", body, sizeof body) != CHAT_OK || is_blank(body)) {
        respond_error(fd, 400, "消息内容不能为空");
        return CHAT_ERR;
    }
    if (strlen(body) > MAX_MSG_LEN) {
        respond_error(fd, 400, "消息内容过长");
        return CHAT_ERR;
    }
    /* 拒绝控制字符与非法 UTF-8：前者会让 JSON 转义放大 6 倍从而撑爆响应缓冲，
     * 后者会入库脏数据，导致严格解析的客户端永久读不出内容 */
    if (!util_valid_text(body)) {
        respond_error(fd, 400, "消息内容包含非法字符");
        return CHAT_ERR;
    }

    rc = normalize_room(raw, &me, room, sizeof room, err, sizeof err);
    if (rc != CHAT_OK) {
        respond_error(fd, rc, err);
        return CHAT_ERR;
    }

    id = db_msg_add(room, me.user_id, me.username, body);
    if (id <= 0) {
        respond_error(fd, 500, "服务器内部错误");
        return CHAT_ERR;
    }

    /* 唤醒所有挂在 /api/poll 上的长轮询：新消息立即送达，不必等超时 */
    notify_ping();

    snprintf(resp, sizeof resp, "{\"ok\":true,\"id\":%d}", id);
    http_respond_json(fd, 200, resp);
    return CHAT_OK;
}

/* GET /api/poll */
int message_poll(const request_t *req, int fd)
{
    session_t me;
    char raw[MAX_ROOM + 1];
    char room[MAX_ROOM + 1];
    char err[64];
    char *msgbuf;
    char *reqbuf = NULL;
    long after_id;
    long last_id;
    long friends_rev = 0;
    long v0;
    int rc;

    if (auth_from_request(req, &me) != CHAT_OK) {
        respond_error(fd, 401, "未登录");
        return CHAT_ERR;
    }
    if (http_query_param(req, "room", raw, sizeof raw) != CHAT_OK) {
        put_str(raw, sizeof raw, ROOM_WORLD);
    }
    after_id = parse_after_id(req);

    rc = normalize_room(raw, &me, room, sizeof room, err, sizeof err);
    if (rc == 400) {
        respond_error(fd, 400, err);       /* 房间名格式非法：仍返回 400 */
        return CHAT_ERR;
    }
    if (rc != CHAT_OK) {
        /* 私聊已失效（非好友 / 不参与 / 游客）：
         * 不能静默改成返回 #world —— 那样客户端会把世界消息当成私聊内容显示。
         * 返回 403，由前端退出该私聊房间。 */
        respond_error(fd, 403, err);
        return CHAT_ERR;
    }

    msgbuf = (char *)malloc(MSG_BUF_SIZE);
    if (msgbuf == NULL) {
        respond_error(fd, 500, "服务器内部错误");
        return CHAT_ERR;
    }

    /* 长轮询：先取一遍；若没有任何新消息 / 新申请，就挂在条件变量上等待，
     * 而不是让前端每秒发一次空请求（白白占用带宽与线程）。
     * v0 在取数之前读取：等待期间（含取数期间）的任何变化都会让
     * notify_wait_until() 立即返回，因此不会漏掉中间发生的更新。 */
    v0 = notify_ver();
    rc = db_msg_fetch_json(room, after_id, msgbuf, MSG_BUF_SIZE);
    if (rc != CHAT_OK) {
        msgbuf[0] = '\0';                  /* 轮询不因消息过多/取消息失败而失败 */
    }
    if (!me.is_guest) {
        reqbuf = (char *)malloc(REQ_BUF_SIZE);
        if (reqbuf != NULL &&
            db_friend_requests_json(me.user_id, reqbuf, REQ_BUF_SIZE) != CHAT_OK) {
            reqbuf[0] = '\0';              /* 好友申请过多也不影响轮询 */
        }
    }

    if (msgbuf[0] == '\0' && (reqbuf == NULL || reqbuf[0] == '\0')) {
        notify_wait_until(v0, CHAT_POLL_WAIT_MS);

        rc = db_msg_fetch_json(room, after_id, msgbuf, MSG_BUF_SIZE);
        if (rc != CHAT_OK) {
            msgbuf[0] = '\0';
        }
        if (!me.is_guest && reqbuf != NULL &&
            db_friend_requests_json(me.user_id, reqbuf, REQ_BUF_SIZE) != CHAT_OK) {
            reqbuf[0] = '\0';
        }
    }

    last_id = last_id_of(msgbuf, after_id);
    if (!me.is_guest) {
        friends_rev = db_friend_rev();
    }

    respond_poll(fd, msgbuf, reqbuf ? reqbuf : "", last_id, friends_rev);
    free(reqbuf);
    free(msgbuf);
    return CHAT_OK;
}
