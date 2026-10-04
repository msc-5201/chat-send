/*
 * db_message.c —— 消息读写
 *
 * 锁约定（与 include/db.h 注释一致）：每个公开函数各自 db_lock()/db_unlock()，
 * 持锁后只通过 db_handle() 取连接；每条 return 路径都先 db_unlock()。
 * 时间戳用 time(NULL)，本地时间字符串交给 SQLite 的 datetime() 生成，
 * 不依赖 src/util.c（并行包 P1）。
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "db.h"
#include "json.h"

/* 列顺序：id, sender_id, sender, body, ts（本地时间字符串） */
#define MSG_COLS \
    "id, sender_id, sender, body," \
    " COALESCE(datetime(created_at, 'unixepoch', 'localtime'), '')"

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

int db_msg_add(const char *room, int sender_id, const char *sender, const char *body)
{
    static const char *const sql =
        "INSERT INTO messages(room, sender_id, sender, body, created_at)"
        " VALUES(?1, ?2, ?3, ?4, ?5)";
    sqlite3_stmt *st = NULL;
    int ret = CHAT_ERR;

    if (room == NULL || sender == NULL || body == NULL) {
        return CHAT_ERR;
    }

    db_lock();
    if (db_handle() == NULL) {
        db_unlock();
        return CHAT_ERR;
    }

    if (sqlite3_prepare_v2(db_handle(), sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, room, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, sender_id);
        sqlite3_bind_text(st, 3, sender, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, body, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 5, (sqlite3_int64)time(NULL));
        if (sqlite3_step(st) == SQLITE_DONE) {
            int id = (int)sqlite3_last_insert_rowid(db_handle());
            if (id > 0) {
                ret = id;                /* 成功时返回新消息 id */
            }
        }
        sqlite3_finalize(st);
    }

    db_unlock();
    return ret;
}

/* 单次查询允许的最大条数。由大到小重试，保证无论消息多长都能给出**合法 JSON**：
 * 宁可在一次响应里少给几条（客户端下次轮询会继续拿），也不要因为放不下而让整个房间报 500。 */
static const int k_fetch_limits[] = { 200, 100, 50, 25, 10, 5, 2, 1 };
#define FETCH_LIMIT_COUNT (sizeof(k_fetch_limits) / sizeof(k_fetch_limits[0]))

/* 按指定条数上限取一次；返回 CHAT_OK 表示成功，CHAT_ERR_FULL 表示缓冲区放不下 */
static int fetch_once(const char *room, long after_id, int limit, char *out, size_t n)
{
    /* after_id <= 0：取最近 limit 条，但输出仍按 id 升序 */
    static const char *const sql_recent =
        "SELECT " MSG_COLS " FROM"
        " (SELECT * FROM messages WHERE room = ?1 ORDER BY id DESC LIMIT ?2)"
        " ORDER BY id ASC";
    /* after_id > 0：取 id 大于它的最早 limit 条 */
    static const char *const sql_after =
        "SELECT " MSG_COLS " FROM messages"
        " WHERE room = ?1 AND id > ?2 ORDER BY id ASC LIMIT ?3";
    sqlite3 *h;
    sqlite3_stmt *st = NULL;
    int step = SQLITE_DONE;
    int ret = CHAT_ERR;

    out[0] = '\0';

    db_lock();
    h = db_handle();
    if (h == NULL) {
        db_unlock();
        return CHAT_ERR;
    }

    if (sqlite3_prepare_v2(h, (after_id > 0) ? sql_after : sql_recent,
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, room, -1, SQLITE_TRANSIENT);
        if (after_id > 0) {
            sqlite3_bind_int64(st, 2, (sqlite3_int64)after_id);
            sqlite3_bind_int(st, 3, limit);
        } else {
            sqlite3_bind_int(st, 2, limit);
        }
        ret = CHAT_OK;
        while ((step = sqlite3_step(st)) == SQLITE_ROW) {
            char sender[MAX_USERNAME + 1];
            char ts[24];
            const char *body;
            char *item;
            size_t cap;
            int id;
            int sender_id;

            id = sqlite3_column_int(st, 0);
            sender_id = sqlite3_column_int(st, 1);
            copy_col(sender, sizeof(sender), st, 2);
            body = (const char *)sqlite3_column_text(st, 3);
            if (body == NULL) {
                body = "";
            }
            copy_col(ts, sizeof(ts), st, 4);

            /* 消息正文可能很长，按实际长度分配「一条消息」的临时缓冲，
             * 在空缓冲里拼好字段（首字段不会多出逗号）再整体追加 */
            cap = 6 * (strlen(sender) + strlen(body)) + 192;
            item = (char *)malloc(cap);
            if (item == NULL) {
                out[0] = '\0';
                ret = CHAT_ERR;
                break;
            }
            item[0] = '\0';
            if (json_append_int(item, cap, "id", id) < 0 ||
                json_append_int(item, cap, "sender_id", sender_id) < 0 ||
                json_append_str(item, cap, "sender", sender) < 0 ||
                json_append_str(item, cap, "body", body) < 0 ||
                json_append_str(item, cap, "ts", ts) < 0 ||
                json_append_raw(out, n, "{") < 0 ||
                raw_append(out, n, item) != CHAT_OK ||
                json_close_object(out, n) != CHAT_OK) {
                free(item);
                out[0] = '\0';
                ret = CHAT_ERR_FULL;
                break;
            }
            free(item);
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

int db_msg_fetch_json(const char *room, long after_id, char *out, size_t n)
{
    size_t i;

    if (room == NULL || out == NULL) {
        return CHAT_ERR;
    }
    if (n == 0) {
        return CHAT_ERR_FULL;
    }
    out[0] = '\0';

    for (i = 0; i < FETCH_LIMIT_COUNT; i++) {
        int rc = fetch_once(room, after_id, k_fetch_limits[i], out, n);

        if (rc == CHAT_OK) {
            return CHAT_OK;              /* 可能少给了若干条，但 JSON 一定合法 */
        }
        if (rc != CHAT_ERR_FULL) {
            return rc;                   /* 真正的错误（例如数据库未打开） */
        }
        /* CHAT_ERR_FULL：条数太多放不下，减半再试 */
    }

    /* 极端情况：单条消息本身就超出缓冲区。返回空列表（合法 JSON）而不是让房间报 500 */
    out[0] = '\0';
    return CHAT_OK;
}
