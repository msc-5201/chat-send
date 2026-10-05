/*
 * db_user.c —— 用户表查询
 *
 * 锁约定：每个公开函数各自 db_lock()/db_unlock()，锁内只通过 db_handle()
 * 取连接，绝不调用另一个会自己加锁的 db_* 公开函数；每条返回路径都先解锁。
 */
#include <string.h>

#include "db.h"

/* ---- 私有小工具 ---- */

/* 拷进定长缓冲，保证 '\0' 结尾且不溢出；放不下返回 CHAT_ERR_FULL，dst 为 NULL 表示不需要 */
static int copy_out(char *dst, size_t n, const char *src)
{
    size_t len;

    if (dst == NULL) {
        return CHAT_OK;
    }
    if (n == 0) {
        return CHAT_ERR_FULL;
    }
    if (src == NULL) {
        src = "";
    }
    len = strlen(src);
    if (len >= n) {
        return CHAT_ERR_FULL;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
    return CHAT_OK;
}

/* 读一列文本并安全拷进定长缓冲（sqlite3_column_text 可能返回 NULL） */
static void copy_col(char *dst, size_t n, sqlite3_stmt *st, int col)
{
    (void)copy_out(dst, n, (const char *)sqlite3_column_text(st, col));
}

#define USER_COLS "id, username, uid, handle"

static void fill_user(user_t *out, sqlite3_stmt *st)
{
    memset(out, 0, sizeof(*out));
    out->id = sqlite3_column_int(st, 0);
    out->is_guest = 0;               /* users 表只存注册用户 */
    copy_col(out->username, sizeof(out->username), st, 1);
    copy_col(out->uid, sizeof(out->uid), st, 2);
    copy_col(out->handle, sizeof(out->handle), st, 3);
}

/* 单参数文本查询；调用方须已持锁并保证 h 非空 */
static int query_user_by_text(sqlite3 *h, const char *sql, const char *arg, user_t *out)
{
    sqlite3_stmt *st = NULL;
    int ret;

    if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) != SQLITE_OK) {
        return CHAT_ERR;
    }
    sqlite3_bind_text(st, 1, arg, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        fill_user(out, st);
        ret = CHAT_OK;
    } else {
        ret = CHAT_ERR_NOTFOUND;
    }
    sqlite3_finalize(st);
    return ret;
}

/* ---- 公开接口 ---- */

int db_user_create(const char *username, const char *pass_hash, const char *salt,
                   const char *uid, const char *handle, user_t *out_user)
{
    static const char *const sql =
        "INSERT INTO users(username, pass_hash, salt, uid, handle, created_at)"
        " VALUES(?, ?, ?, ?, ?, CAST(strftime('%s','now') AS INTEGER))";
    sqlite3 *h;
    sqlite3_stmt *st = NULL;
    int ret = CHAT_ERR;

    if (username == NULL || pass_hash == NULL || salt == NULL ||
        uid == NULL || handle == NULL) {
        return CHAT_ERR;
    }

    db_lock();
    h = db_handle();
    if (h == NULL) {
        db_unlock();
        return CHAT_ERR;
    }
    if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, username, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, pass_hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, salt, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, uid, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 5, handle, -1, SQLITE_TRANSIENT);

        switch (sqlite3_step(st)) {
        case SQLITE_DONE:
            ret = CHAT_OK;
            if (out_user != NULL) {
                memset(out_user, 0, sizeof(*out_user));
                out_user->id = (int)sqlite3_last_insert_rowid(h);
                out_user->is_guest = 0;
                (void)copy_out(out_user->username, sizeof(out_user->username), username);
                (void)copy_out(out_user->uid, sizeof(out_user->uid), uid);
                (void)copy_out(out_user->handle, sizeof(out_user->handle), handle);
            }
            break;
        case SQLITE_CONSTRAINT:
            ret = CHAT_ERR_TAKEN;    /* 用户名 / uid / handle 已存在 */
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

int db_user_by_username(const char *username, user_t *out,
                        char *hash, size_t hash_n, char *salt, size_t salt_n)
{
    static const char *const sql =
        "SELECT " USER_COLS ", pass_hash, salt FROM users WHERE username = ?";
    sqlite3 *h;
    sqlite3_stmt *st = NULL;
    int ret = CHAT_ERR;

    if (username == NULL) {
        return CHAT_ERR_NOTFOUND;
    }

    db_lock();
    h = db_handle();
    if (h == NULL) {
        db_unlock();
        return CHAT_ERR_NOTFOUND;
    }
    if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, username, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *ph = (const char *)sqlite3_column_text(st, 4);
            const char *ps = (const char *)sqlite3_column_text(st, 5);
            ret = CHAT_OK;
            if (copy_out(hash, hash_n, ph) != CHAT_OK) {
                ret = CHAT_ERR_FULL;
            }
            if (copy_out(salt, salt_n, ps) != CHAT_OK) {
                ret = CHAT_ERR_FULL;
            }
            if (out != NULL) {
                fill_user(out, st);
            }
        } else {
            ret = CHAT_ERR_NOTFOUND;
        }
        sqlite3_finalize(st);
    }
    db_unlock();
    return ret;
}

int db_user_by_uid(const char *uid, user_t *out)
{
    static const char *const sql = "SELECT " USER_COLS " FROM users WHERE uid = ?";
    sqlite3 *h;
    int ret;

    if (uid == NULL || out == NULL) {
        return CHAT_ERR_NOTFOUND;
    }

    db_lock();
    h = db_handle();
    if (h == NULL) {
        db_unlock();
        return CHAT_ERR_NOTFOUND;
    }
    ret = query_user_by_text(h, sql, uid, out);
    db_unlock();
    return ret;
}

int db_user_by_handle(const char *handle, user_t *out)
{
    static const char *const sql = "SELECT " USER_COLS " FROM users WHERE handle = ?";
    sqlite3 *h;
    int ret;

    if (handle == NULL || out == NULL) {
        return CHAT_ERR_NOTFOUND;
    }

    db_lock();
    h = db_handle();
    if (h == NULL) {
        db_unlock();
        return CHAT_ERR_NOTFOUND;
    }
    ret = query_user_by_text(h, sql, handle, out);
    db_unlock();
    return ret;
}

int db_user_by_id(int id, user_t *out)
{
    static const char *const sql = "SELECT " USER_COLS " FROM users WHERE id = ?";
    sqlite3 *h;
    sqlite3_stmt *st = NULL;
    int ret = CHAT_ERR_NOTFOUND;

    if (out == NULL) {
        return CHAT_ERR;
    }

    db_lock();
    h = db_handle();
    if (h == NULL) {
        db_unlock();
        return CHAT_ERR_NOTFOUND;
    }
    if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            fill_user(out, st);
            ret = CHAT_OK;
        } else {
            ret = CHAT_ERR_NOTFOUND;
        }
        sqlite3_finalize(st);
    }
    db_unlock();
    return ret;
}

int db_user_set_handle(int id, const char *handle)
{
    static const char *const sql = "UPDATE users SET handle = ? WHERE id = ?";
    sqlite3 *h;
    sqlite3_stmt *st = NULL;
    int ret = CHAT_ERR;

    if (handle == NULL) {
        return CHAT_ERR;
    }

    db_lock();
    h = db_handle();
    if (h == NULL) {
        db_unlock();
        return CHAT_ERR;
    }
    if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, handle, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, id);
        switch (sqlite3_step(st)) {
        case SQLITE_DONE:
            ret = (sqlite3_changes(h) == 0) ? CHAT_ERR_NOTFOUND : CHAT_OK;
            break;
        case SQLITE_CONSTRAINT:
            ret = CHAT_ERR_TAKEN;    /* 身份码已被别人占用 */
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

int db_user_handle_taken(const char *handle, int exclude_id)
{
    static const char *const sql_all = "SELECT 1 FROM users WHERE handle = ? LIMIT 1";
    static const char *const sql_ex =
        "SELECT 1 FROM users WHERE handle = ? AND id <> ? LIMIT 1";
    sqlite3 *h;
    sqlite3_stmt *st = NULL;
    int taken = 0;

    if (handle == NULL) {
        return 0;
    }

    db_lock();
    h = db_handle();
    if (h == NULL) {
        db_unlock();
        return 0;
    }
    if (sqlite3_prepare_v2(h, (exclude_id < 0) ? sql_all : sql_ex, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, handle, -1, SQLITE_TRANSIENT);
        if (exclude_id >= 0) {
            sqlite3_bind_int(st, 2, exclude_id);
        }
        if (sqlite3_step(st) == SQLITE_ROW) {
            taken = 1;
        }
        sqlite3_finalize(st);
    }
    db_unlock();
    return taken;
}

int db_user_set_password(int id, const char *pass_hash, const char *salt)
{
    static const char *const sql =
        "UPDATE users SET pass_hash = ?1, salt = ?2 WHERE id = ?3";
    sqlite3 *h;
    sqlite3_stmt *st = NULL;
    int ret = CHAT_ERR;

    if (pass_hash == NULL || salt == NULL) {
        return CHAT_ERR;
    }

    db_lock();
    h = db_handle();
    if (h == NULL) {
        db_unlock();
        return CHAT_ERR;
    }
    if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, pass_hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, salt, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 3, id);
        if (sqlite3_step(st) == SQLITE_DONE) {
            /* changes()==0 说明 id 不存在（或新旧哈希完全相同，不会发生：每次都换新盐） */
            ret = (sqlite3_changes(h) == 0) ? CHAT_ERR_NOTFOUND : CHAT_OK;
        }
        sqlite3_finalize(st);
    }
    db_unlock();
    return ret;
}

int db_user_stats(int user_id, long *created_at, int *friend_count, int *message_count)
{
    static const char *const sql_created =
        "SELECT created_at FROM users WHERE id = ?1";
    static const char *const sql_friends =
        "SELECT COUNT(*) FROM friends WHERE user_id = ?1";
    static const char *const sql_messages =
        "SELECT COUNT(*) FROM messages WHERE sender_id = ?1";
    sqlite3 *h;
    sqlite3_stmt *st = NULL;
    int ret = CHAT_ERR_NOTFOUND;

    if (created_at == NULL || friend_count == NULL || message_count == NULL) {
        return CHAT_ERR;
    }
    *created_at = 0;
    *friend_count = 0;
    *message_count = 0;

    db_lock();
    h = db_handle();
    if (h == NULL) {
        db_unlock();
        return CHAT_ERR;
    }

    /* 1) 注册时间：查不到即视为用户不存在，直接返回 */
    if (sqlite3_prepare_v2(h, sql_created, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, user_id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            *created_at = (long)sqlite3_column_int64(st, 0);
            ret = CHAT_OK;
        }
        sqlite3_finalize(st);
        st = NULL;
    }

    /* 2) 好友数 */
    if (ret == CHAT_OK && sqlite3_prepare_v2(h, sql_friends, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, user_id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            *friend_count = sqlite3_column_int(st, 0);
        }
        sqlite3_finalize(st);
        st = NULL;
    }

    /* 3) 发言数（世界 + 私聊，按 sender_id 统计） */
    if (ret == CHAT_OK && sqlite3_prepare_v2(h, sql_messages, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, user_id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            *message_count = sqlite3_column_int(st, 0);
        }
        sqlite3_finalize(st);
        st = NULL;
    }

    db_unlock();
    return ret;
}
