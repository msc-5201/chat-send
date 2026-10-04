/*
 * friend.c —— 好友路由（真实实现，包 P5）
 *
 * 覆盖：好友列表 / 发送申请 / 同意 / 拒绝。
 * 状态码与 JSON 结构遵循 API.md「2. 好友」章节；所有响应由本文件自行发送。
 */
#include "friend.h"

#include "auth.h"
#include "db.h"
#include "http.h"
#include "json.h"
#include "util.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 好友 / 申请两段 JSON 片段的独立上限；外层响应另留余量 */
#define FRAG_BUF 8192
#define RESP_BUF 32768

/* 用 json_append_* 拼好的字段串外包一层花括号，避免手写转义 */
static int respond_object(int fd, int status, const char *fields)
{
    char body[4096];

    if (snprintf(body, sizeof(body), "{%s}", fields) < 0) {
        http_respond_json(fd, 500, "{\"ok\":false,\"error\":\"内部错误\"}");
        return CHAT_ERR;
    }
    http_respond_json(fd, status, body);
    return CHAT_OK;
}

/* 统一错误响应：{"ok":false,"error":"<msg>"} */
static int fail(int fd, int status, const char *msg)
{
    char fields[512];

    fields[0] = '\0';
    if (json_append_bool(fields, sizeof(fields), "ok", 0) < 0 ||
        json_append_str(fields, sizeof(fields), "error", msg) < 0) {
        http_respond_json(fd, 500, "{\"ok\":false,\"error\":\"内部错误\"}");
        return CHAT_ERR;
    }
    return respond_object(fd, status, fields);
}

/* 统一成功响应：{"ok":true,"message":"<msg>"} */
static int ok_message(int fd, const char *msg)
{
    char fields[512];

    fields[0] = '\0';
    if (json_append_bool(fields, sizeof(fields), "ok", 1) < 0 ||
        json_append_str(fields, sizeof(fields), "message", msg) < 0) {
        http_respond_json(fd, 500, "{\"ok\":false,\"error\":\"内部错误\"}");
        return CHAT_ERR;
    }
    return respond_object(fd, 200, fields);
}

/* 公共前提：解析会话并拒绝游客。成功返回 CHAT_OK 并填写 me；
 * 失败时已自行发送 401 / 403 响应，调用方直接 return。 */
static int require_user(const request_t *req, int fd, session_t *me)
{
    if (auth_from_request(req, me) != CHAT_OK) {
        return fail(fd, 401, "未登录");
    }
    if (me->is_guest) {
        return fail(fd, 403, "游客不能使用好友功能");
    }
    return CHAT_OK;
}

/* 严格解析正整数：全部为数字且不超 int 范围。合法返回 CHAT_OK 并写 *out */
static int parse_positive_int(const char *s, int *out)
{
    long v;
    char *end;
    size_t i;

    if (s == NULL || s[0] == '\0') {
        return CHAT_ERR;
    }
    for (i = 0; s[i] != '\0'; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return CHAT_ERR;
        }
    }
    v = strtol(s, &end, 10);
    if (*end != '\0' || v <= 0 || v > INT_MAX) {
        return CHAT_ERR;
    }
    *out = (int)v;
    return CHAT_OK;
}

int friend_list(const request_t *req, int fd)
{
    session_t me;
    char friends[FRAG_BUF];
    char requests[FRAG_BUF];
    char body[RESP_BUF];
    int  r1;
    int  r2;
    int  n;

    if (require_user(req, fd, &me) != CHAT_OK) {
        return CHAT_ERR;
    }

    r1 = db_friend_list_json(me.user_id, friends, sizeof(friends));
    if (r1 == CHAT_ERR_FULL) {
        fprintf(stderr, "friend_list: 好友列表片段超出缓冲\n");
        return fail(fd, 500, "好友数据过大");
    }
    r2 = db_friend_requests_json(me.user_id, requests, sizeof(requests));
    if (r2 == CHAT_ERR_FULL) {
        fprintf(stderr, "friend_list: 申请列表片段超出缓冲\n");
        return fail(fd, 500, "好友数据过大");
    }
    if (r1 != CHAT_OK || r2 != CHAT_OK) {
        fprintf(stderr, "friend_list: 读取好友数据失败\n");
        return fail(fd, 500, "读取好友数据失败");
    }

    /* db_*_json 只产出数组元素片段（不含方括号）；空串必须写成 []。
     * 这里用 snprintf 手工包裹外层结构，避免 json_append_raw 在非空缓冲后补逗号。 */
    n = snprintf(body, sizeof(body),
                 "{\"ok\":true,\"friends\":[%s],\"requests\":[%s]}",
                 friends, requests);
    if (n < 0 || (size_t)n >= sizeof(body)) {
        fprintf(stderr, "friend_list: 响应超出缓冲\n");
        return fail(fd, 500, "好友数据过大");
    }

    http_respond_json(fd, 200, body);
    return CHAT_OK;
}

int friend_add(const request_t *req, int fd)
{
    session_t me;
    user_t target;
    /* 故意大于 MAX_HANDLE：让超长输入被校验拒绝，而不是被 http_param 静默截断后"变合法" */
    char handle[128];
    int r;

    if (require_user(req, fd, &me) != CHAT_OK) {
        return CHAT_ERR;
    }

    if (http_param(req, "handle", handle, sizeof(handle)) != CHAT_OK ||
        handle[0] == '\0') {
        return fail(fd, 400, "请输入对方的身份码");
    }
    if (!util_valid_handle(handle)) {
        return fail(fd, 400, "身份码格式不正确");
    }

    r = db_user_by_handle(handle, &target);
    if (r == CHAT_ERR_NOTFOUND) {
        return fail(fd, 404, "找不到该身份码对应的用户");
    }
    if (r != CHAT_OK) {
        return fail(fd, 400, "无法添加该用户");
    }

    if (target.id == me.user_id) {
        return fail(fd, 400, "不能添加自己为好友");
    }

    r = db_friend_add_request(me.user_id, target.id);
    if (r == CHAT_ERR_CONFLICT) {
        return fail(fd, 409, "已经是好友或申请已发送");
    }
    if (r != CHAT_OK) {
        return fail(fd, 400, "无法添加该用户");
    }

    return ok_message(fd, "好友申请已发送");
}

/* accept / reject 共用逻辑：accept 为 1 表示同意，0 表示拒绝 */
static int friend_decide(const request_t *req, int fd, int accept)
{
    session_t me;
    char idbuf[32];
    int  req_id;
    int  r;

    if (require_user(req, fd, &me) != CHAT_OK) {
        return CHAT_ERR;
    }

    if (http_param(req, "id", idbuf, sizeof(idbuf)) != CHAT_OK ||
        parse_positive_int(idbuf, &req_id) != CHAT_OK) {
        return fail(fd, 400, "申请编号不正确");
    }

    r = db_friend_set_request(req_id, me.user_id, accept);
    if (r != CHAT_OK) {
        return fail(fd, 404, "申请不存在或无权处理");
    }

    http_respond_json(fd, 200, "{\"ok\":true}");
    return CHAT_OK;
}

int friend_accept(const request_t *req, int fd)
{
    return friend_decide(req, fd, 1);
}

int friend_reject(const request_t *req, int fd)
{
    return friend_decide(req, fd, 0);
}
