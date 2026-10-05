/*
 * chat.h —— 全项目公共类型与常量（契约文件，冻结后只读）
 */
#ifndef CHAT_H
#define CHAT_H

#include <stddef.h>

/* ---- 服务器配置（可在编译时用 -D 覆盖，便于各包并行起独立测试实例） ---- */
#ifndef CHAT_PORT
#define CHAT_PORT       8080
#endif
#ifndef CHAT_DB_PATH
#define CHAT_DB_PATH    "chat.db"
#endif
#ifndef CHAT_STATIC_ROOT
#define CHAT_STATIC_ROOT "public"
#endif

/* ---- 仪表盘数据根目录 ----
 * 每个注册用户一份目录：<CHAT_DASHBOARD_ROOT>/<用户名>/<用户名>.json
 * （形如 Dashboard/alice/alice.json）。与 CHAT_STATIC_ROOT / CHAT_DB_PATH 一样
 * 按「可执行文件所在目录」解析（main.c 启动时会切工作目录）。 */
#ifndef CHAT_DASHBOARD_ROOT
#define CHAT_DASHBOARD_ROOT "Dashboard"
#endif

/* ---- 长轮询等待上限（毫秒） ----
 * 前端不再每秒空轮询：/api/poll 与 /api/transfer/poll 在没有新数据时
 * 阻塞等待（见 notify.h），有新数据立即返回，否则最多等这么久。
 * 编译期可覆盖（如测试用 -DCHAT_POLL_WAIT_MS=300 缩短等待）。 */
#ifndef CHAT_POLL_WAIT_MS
#define CHAT_POLL_WAIT_MS 25000
#endif

/* ---- 世界聊天房间名 ---- */
#define ROOM_WORLD      "#world"

/* ---- 字段长度上限（均不含结尾 '\0'） ---- */
#define MAX_USERNAME    32
#define MAX_HANDLE      32
#define MAX_UID         16
#define MAX_TOKEN       64
#define MAX_ROOM        32
#define MAX_MSG_LEN     2000
#define MAX_BODY_LEN    8192
#define MAX_QUERY       2048
#define MAX_COOKIE      1024
#define MAX_PATH        256

/* ---- 身份角色 ---- */
#define USER_MODE_NORMAL 0
#define USER_MODE_GUEST  1

/* ---- 统一返回码 ---- */
#define CHAT_OK            0
#define CHAT_ERR          (-1)
#define CHAT_ERR_TAKEN    (-2)   /* 用户名 / 身份码 / 标识码已存在 */
#define CHAT_ERR_NOTFOUND (-3)
#define CHAT_ERR_CONFLICT (-4)
#define CHAT_ERR_FULL     (-5)   /* 输出缓冲区不足 */

/* 注册用户：uid 为标识码（不可改），handle 为身份码（可改，用于加好友） */
typedef struct {
    int  id;
    int  is_guest;
    char username[MAX_USERNAME + 1];
    char uid[MAX_UID + 1];
    char handle[MAX_HANDLE + 1];
} user_t;

/* 一个已解析的 HTTP 请求 */
typedef struct {
    char method[8];
    char path[MAX_PATH + 1];
    char query[MAX_QUERY + 1];
    char cookie[MAX_COOKIE + 1];
    char body[MAX_BODY_LEN + 1];
} request_t;

/* 一次登录会话（游客也有会话，只是 is_guest = 1） */
typedef struct {
    int  user_id;
    int  is_guest;
    char token[MAX_TOKEN + 1];
    char uid[MAX_UID + 1];
    char handle[MAX_HANDLE + 1];
    char username[MAX_USERNAME + 1];
} session_t;

#endif /* CHAT_H */
