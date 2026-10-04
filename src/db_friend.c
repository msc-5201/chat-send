/*
 * db_friend.c —— 好友申请与好友关系
 *
 * 锁约定（与 include/db.h 注释一致）：每个公开函数各自 db_lock()/db_unlock()，
 * 持锁后只通过 db_handle() 取连接；内部辅助函数一律不加锁，也绝不调用另一个
 * 会自己加锁的 db_* 公开函数。每条 return 路径都先 db_unlock()。
 */
#include <string.h>
#include <time.h>

#include "db.h"
#include "json.h"

/* 读一列文本并安全拷进定长缓冲（sqlite3_column_text 可能返回 NULL） */
static void copy_col(char *dst, size_t n, sqlite3_stmt *st, int col)
{
    const char *s = (const char *)sqlite3_column_text(st, col);
    size_t len;

    if (dst == NULL || n == 0) {
        return;
    }
    if (s == NULL) {
        s = "";
    }
    len = strlen(s);
    if (len >= n) {
        len = n - 1;                 /* 截断但保证 '\0' 结尾 */
    }
    memcpy(dst, s, len);
    dst[len] = '\0';
}

static const char *const SQL_FRIEND_EXISTS =
    "SELECT 1 FROM friends WHERE user_id = ?1 AND friend_id = ?2 LIMIT 1";

static const char *const SQL_PENDING_FROM =
    "SELECT 1 FROM friend_requests WHERE from_user = ?1 AND to_user = ?2"
    " AND status = 'pending' LIMIT 1";

/* 单个好友/申请对象的临时缓冲：username+handle 转义后最长 2*32*6=384，
 * uid 最长 16*6=96，键名与数字约 100，768 足够。 */
#define ITEM_MAX 768

/* 判断「带两个 int 参数」的查询是否至少返回一行：1 有，0 无，-1 出错。
 * 调用方必须已持锁。 */
static int row_exists2(sqlite3 *h, const char *sql, int a, int b)
{
    sqlite3_stmt *st = NULL;
    int ret;

    if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int(st, 1, a);
    sqlite3_bind_int(st, 2, b);
    ret = (sqlite3_step(st) == SQLITE_ROW) ? 1 : 0;
    sqlite3_finalize(st);
    return ret;
}

/* 双向写入好友关系（幂等）。调用方必须已持锁。 */
static int friend_link(sqlite3 *h, int a, int b)
{
    static const char *const sql =
        "INSERT OR IGNORE INTO friends(user_id, friend_id, created_at)"
        " VALUES(?1, ?2, ?3)";
    sqlite3_stmt *st = NULL;
    int ret = CHAT_OK;

    if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) != SQLITE_OK) {
        return CHAT_ERR;
    }
    sqlite3_bind_int(st, 1, a);
    sqlite3_bind_int(st, 2, b);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)time(NULL));
    if (sqlite3_step(st) != SQLITE_DONE) {
        ret = CHAT_ERR;
    } else {
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        sqlite3_bind_int(st, 1, b);
        sqlite3_bind_int(st, 2, a);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)time(NULL));
        if (sqlite3_step(st) != SQLITE_DONE) {
            ret = CHAT_ERR;
        }
    }
    sqlite3_finalize(st);
    return ret;
}

/* 把申请状态从 pending 改成 status（只有仍在 pending 的会被改动）。
 * 调用方必须已持锁。 */
static int request_set_status(sqlite3 *h, int req_id, const char *status)
{
    static const char *const sql =
        "UPDATE friend_requests SET status = ?1 WHERE id = ?2 AND status = 'pending'";
    sqlite3_stmt *st = NULL;
    int ret = CHAT_ERR;

    if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, status, -1, SQLITE_STATIC);
        sqlite3_bind_int(st, 2, req_id);
        if (sqlite3_step(st) == SQLITE_DONE) {
            ret = CHAT_OK;
        }
        sqlite3_finalize(st);
    }
    return ret;
}

/* 同意申请：先建好友关系（幂等），再把申请标记为 accepted。
 * 先写 friends 后改状态，中途失败时申请仍是 pending，可重试且不漏好友。
 * 调用方必须已持锁。 */
static int request_accept(sqlite3 *h, int req_id, int from_id, int to_id)
{
    if (friend_link(h, from_id, to_id) != CHAT_OK) {
        return CHAT_ERR;
    }
    return request_set_status(h, req_id, "accepted");
}

/* 互相申请：对方先前发来的 pending 申请直接转成 accepted，双方成为好友。
 * 调用方必须已持锁。 */
static int request_accept_mutual(sqlite3 *h, int from_id, int to_id)
{
    sqlite3_stmt *st = NULL;
    int ret = CHAT_ERR;

    if (sqlite3_prepare_v2(h,
            "UPDATE friend_requests SET status = 'accepted'"
            " WHERE from_user = ?1 AND to_user = ?2 AND status = 'pending'",
            -1, &st, NULL) != SQLITE_OK) {
        return CHAT_ERR;
    }
    sqlite3_bind_int(st, 1, to_id);
    sqlite3_bind_int(st, 2, from_id);
    if (sqlite3_step(st) == SQLITE_DONE) {
        ret = CHAT_OK;
    }
    sqlite3_finalize(st);

    if (ret == CHAT_OK) {
        ret = friend_link(h, from_id, to_id);
    }
    return ret;
}

/* 追加原始片段但不补逗号（用于把已拼好的对象内容接在 '{' 之后）。 */
static int raw_append(char *out, size_t n, const char *s)
{
    size_t len = strlen(out);
    size_t add = strlen(s);

    if (len + add + 1 > n) {
        return CHAT_ERR_FULL;
    }
    memcpy(out + len, s, add + 1);
    return CHAT_OK;
}

/* 追加对象右花括号。json_append_raw 会在非空出参前补逗号，故单独处理。 */
static int json_close_object(char *out, size_t n)
{
    size_t len = strlen(out);

    if (len + 2 > n) {
        return CHAT_ERR_FULL;
    }
    out[len] = '}';
    out[len + 1] = '\0';
    return CHAT_OK;
}

int db_friend_add_request(int from_id, int to_id)
{
    sqlite3 *h;
    int exists;
    int ret;

    if (from_id == to_id) {
        return CHAT_ERR;                 /* 不能加自己 */
    }

    db_lock();
    h = db_handle();
    if (h == NULL) {
        db_unlock();
        return CHAT_ERR;
    }

    exists = row_exists2(h, SQL_FRIEND_EXISTS, from_id, to_id);
    if (exists < 0) {
        ret = CHAT_ERR;
    } else if (exists) {
        ret = CHAT_ERR_CONFLICT;         /* 已经是好友 */
    } else {
        exists = row_exists2(h, SQL_PENDING_FROM, from_id, to_id);
        if (exists < 0) {
            ret = CHAT_ERR;
        } else if (exists) {
            ret = CHAT_ERR_CONFLICT;     /* 已发过申请，不重复插入 */
        } else {
            exists = row_exists2(h, SQL_PENDING_FROM, to_id, from_id);
            if (exists < 0) {
                ret = CHAT_ERR;
            } else if (exists) {
                /* 对方也给我发过 pending：互相申请，直接成为好友 */
                ret = request_accept_mutual(h, from_id, to_id);
            } else {
                static const char *const sql =
                    "INSERT INTO friend_requests(from_user, to_user, status, created_at)"
                    " VALUES(?1, ?2, 'pending', ?3)";
                sqlite3_stmt *st = NULL;

                ret = CHAT_ERR;
                if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) == SQLITE_OK) {
                    sqlite3_bind_int(st, 1, from_id);
                    sqlite3_bind_int(st, 2, to_id);
                    sqlite3_bind_int64(st, 3, (sqlite3_int64)time(NULL));
                    if (sqlite3_step(st) == SQLITE_DONE) {
                        ret = CHAT_OK;
                    }
                    sqlite3_finalize(st);
                }
            }
        }
    }

    db_unlock();
    return ret;
}

int db_friend_requests_json(int user_id, char *out, size_t n)
{
    static const char *const sql =
        "SELECT r.id, r.from_user, u.username, u.handle"
        " FROM friend_requests r LEFT JOIN users u ON u.id = r.from_user"
        " WHERE r.to_user = ?1 AND r.status = 'pending'"
        " ORDER BY r.id ASC";
    sqlite3 *h;
    sqlite3_stmt *st = NULL;
    int step = SQLITE_DONE;
    int ret = CHAT_ERR;

    if (out == NULL || n == 0) {
        return CHAT_ERR_FULL;
    }
    out[0] = '\0';

    db_lock();
    h = db_handle();
    if (h == NULL) {
        db_unlock();
        return CHAT_ERR;
    }

    if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, user_id);
        ret = CHAT_OK;
        while ((step = sqlite3_step(st)) == SQLITE_ROW) {
            char username[MAX_USERNAME + 1];
            char handle[MAX_HANDLE + 1];
            char item[ITEM_MAX];
            int id;
            int from_id;

            id = sqlite3_column_int(st, 0);
            from_id = sqlite3_column_int(st, 1);
            copy_col(username, sizeof(username), st, 2);   /* JOIN 不到时为空串 */
            copy_col(handle, sizeof(handle), st, 3);

            /* 先在空缓冲里拼字段（json_append_* 只在非空缓冲前补逗号，
             * 首字段才不会多出逗号），再整体作为一个数组元素追加 */
            item[0] = '\0';
            if (json_append_int(item, sizeof(item), "id", id) < 0 ||
                json_append_int(item, sizeof(item), "from_id", from_id) < 0 ||
                json_append_str(item, sizeof(item), "username", username) < 0 ||
                json_append_str(item, sizeof(item), "handle", handle) < 0 ||
                json_append_raw(out, n, "{") < 0 ||
                raw_append(out, n, item) != CHAT_OK ||
                json_close_object(out, n) != CHAT_OK) {
                out[0] = '\0';
                ret = CHAT_ERR_FULL;
                break;
            }
        }
        if (ret == CHAT_OK && step != SQLITE_DONE) {
            out[0] = '\0';
            ret = CHAT_ERR;
        }
        sqlite3_finalize(st);
    }

    db_unlock();
    return ret;
}

int db_friend_set_request(int req_id, int user_id, int accept)
{
    static const char *const sql =
        "SELECT from_user, to_user FROM friend_requests"
        " WHERE id = ?1 AND status = 'pending'";
    sqlite3 *h;
    sqlite3_stmt *st = NULL;
    int from_id = 0;
    int to_id = 0;
    int found = 0;
    int ret = CHAT_ERR;

    if (req_id <= 0) {
        return CHAT_ERR;
    }

    db_lock();
    h = db_handle();
    if (h == NULL) {
        db_unlock();
        return CHAT_ERR;
    }

    if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, req_id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            from_id = sqlite3_column_int(st, 0);
            to_id = sqlite3_column_int(st, 1);
            found = 1;
        }
        sqlite3_finalize(st);
    }

    /* 只有收件人本人可以处理；申请不存在或已处理过都返回 CHAT_ERR */
    if (found && to_id == user_id) {
        ret = accept ? request_accept(h, req_id, from_id, to_id)
                     : request_set_status(h, req_id, "rejected");
    }

    db_unlock();
    return ret;
}

int db_friend_list_json(int user_id, char *out, size_t n)
{
    static const char *const sql =
        "SELECT u.id, u.username, u.uid, u.handle"
        " FROM friends f JOIN users u ON u.id = f.friend_id"
        " WHERE f.user_id = ?1"
        " ORDER BY f.created_at ASC, u.id ASC";
    sqlite3 *h;
    sqlite3_stmt *st = NULL;
    int step = SQLITE_DONE;
    int ret = CHAT_ERR;

    if (out == NULL || n == 0) {
        return CHAT_ERR_FULL;
    }
    out[0] = '\0';

    db_lock();
    h = db_handle();
    if (h == NULL) {
        db_unlock();
        return CHAT_ERR;
    }

    if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, user_id);
        ret = CHAT_OK;
        while ((step = sqlite3_step(st)) == SQLITE_ROW) {
            char username[MAX_USERNAME + 1];
            char uid[MAX_UID + 1];
            char handle[MAX_HANDLE + 1];
            char item[ITEM_MAX];
            int id;

            id = sqlite3_column_int(st, 0);
            copy_col(username, sizeof(username), st, 1);
            copy_col(uid, sizeof(uid), st, 2);
            copy_col(handle, sizeof(handle), st, 3);

            item[0] = '\0';
            if (json_append_int(item, sizeof(item), "id", id) < 0 ||
                json_append_str(item, sizeof(item), "username", username) < 0 ||
                json_append_str(item, sizeof(item), "uid", uid) < 0 ||
                json_append_str(item, sizeof(item), "handle", handle) < 0 ||
                /* 好友只可能是注册用户：游客不在 users 表里，也无法被加为好友 */
                json_append_bool(item, sizeof(item), "is_guest", 0) < 0 ||
                json_append_raw(out, n, "{") < 0 ||
                raw_append(out, n, item) != CHAT_OK ||
                json_close_object(out, n) != CHAT_OK) {
                out[0] = '\0';
                ret = CHAT_ERR_FULL;
                break;
            }
        }
        if (ret == CHAT_OK && step != SQLITE_DONE) {
            out[0] = '\0';
            ret = CHAT_ERR;
        }
        sqlite3_finalize(st);
    }

    db_unlock();
    return ret;
}

int db_friend_are(int a, int b)
{
    sqlite3 *h;
    int exists;
    int ret;

    db_lock();
    h = db_handle();
    if (h == NULL) {
        db_unlock();
        return CHAT_ERR_NOTFOUND;
    }
    exists = row_exists2(h, SQL_FRIEND_EXISTS, a, b);
    ret = (exists == 1) ? CHAT_OK : CHAT_ERR_NOTFOUND;
    db_unlock();
    return ret;
}
