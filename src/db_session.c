/*
 * db_session.c —— 会话表查询
 *
 * created_at / last_seen 由 SQLite 的 strftime('%s','now') 生成，不依赖 src/util.c。
 * 锁约定：每个公开函数各自 db_lock()/db_unlock()，锁内不调用其它会加锁的 db_* 函数。
 */
#include <string.h>

#include "db.h"

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

int db_session_create(const char *token, int user_id, int is_guest,
                      const char *uid, const char *handle, const char *username)
{
    static const char *const sql =
        "INSERT INTO sessions(token, user_id, is_guest, uid, handle, username,"
        " created_at, last_seen)"
        " VALUES(?, ?, ?, ?, ?, ?, CAST(strftime('%s','now') AS INTEGER),"
        " CAST(strftime('%s','now') AS INTEGER))";
    sqlite3 *h;
    sqlite3_stmt *st = NULL;
    int ret = CHAT_ERR;

    if (token == NULL || uid == NULL || username == NULL) {
        return CHAT_ERR;
    }
    if (handle == NULL) {
        handle = "";
    }

    db_lock();
    h = db_handle();
    if (h == NULL) {
        db_unlock();
        return CHAT_ERR;
    }
    if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, token, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, user_id);
        sqlite3_bind_int(st, 3, is_guest);
        sqlite3_bind_text(st, 4, uid, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 5, handle, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 6, username, -1, SQLITE_TRANSIENT);
        switch (sqlite3_step(st)) {
        case SQLITE_DONE:
            ret = CHAT_OK;
            break;
        case SQLITE_CONSTRAINT:
            ret = CHAT_ERR_TAKEN;    /* token 重复 */
            break;
        default:
            ret = CHAT_ERR;
            break;
        }
        sqlite3_finalize(st);
    }
    db_unlock();
    return ret;
}

int db_session_get(const char *token, session_t *out)
{
    static const char *const sql =
        "SELECT token, user_id, is_guest, uid, handle, username"
        " FROM sessions WHERE token = ?";
    sqlite3 *h;
    sqlite3_stmt *st = NULL;
    int ret = CHAT_ERR_NOTFOUND;

    if (token == NULL || out == NULL) {
        return CHAT_ERR_NOTFOUND;
    }

    db_lock();
    h = db_handle();
    if (h == NULL) {
        db_unlock();
        return CHAT_ERR_NOTFOUND;
    }
    if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, token, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            memset(out, 0, sizeof(*out));
            copy_col(out->token, sizeof(out->token), st, 0);
            out->user_id = sqlite3_column_int(st, 1);
            out->is_guest = sqlite3_column_int(st, 2);
            copy_col(out->uid, sizeof(out->uid), st, 3);
            copy_col(out->handle, sizeof(out->handle), st, 4);
            copy_col(out->username, sizeof(out->username), st, 5);
            ret = CHAT_OK;
        } else {
            ret = CHAT_ERR_NOTFOUND;
        }
        sqlite3_finalize(st);
    }
    db_unlock();
    return ret;
}

void db_session_touch(const char *token)
{
    static const char *const sql =
        "UPDATE sessions SET last_seen = CAST(strftime('%s','now') AS INTEGER)"
        " WHERE token = ?";
    sqlite3 *h;
    sqlite3_stmt *st = NULL;

    if (token == NULL) {
        return;
    }

    db_lock();
    h = db_handle();
    if (h == NULL) {
        db_unlock();
        return;
    }
    if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, token, -1, SQLITE_TRANSIENT);
        (void)sqlite3_step(st);
        sqlite3_finalize(st);
    }
    db_unlock();
}

void db_session_delete(const char *token)
{
    static const char *const sql = "DELETE FROM sessions WHERE token = ?";
    sqlite3 *h;
    sqlite3_stmt *st = NULL;

    if (token == NULL) {
        return;
    }

    db_lock();
    h = db_handle();
    if (h == NULL) {
        db_unlock();
        return;
    }
    if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, token, -1, SQLITE_TRANSIENT);
        (void)sqlite3_step(st);
        sqlite3_finalize(st);
    }
    db_unlock();
}

void db_session_clear_guests(void)
{
    sqlite3 *h;

    db_lock();
    h = db_handle();
    if (h == NULL) {
        db_unlock();
        return;
    }
    sqlite3_exec(h, "DELETE FROM sessions WHERE is_guest = 1", NULL, NULL, NULL);
    db_unlock();
}
