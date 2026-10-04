/*
 * db_smoke.c —— 数据库层自测
 *
 * 覆盖：连接/建表幂等、用户增查改、唯一约束冲突、哈希/盐回传与溢出、
 *       会话增删查改、游客会话清理。
 * 运行：.\build\db_smoke.exe  （在 test_web 目录下）
 * 退出码：0 全部通过；非 0 存在失败项。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
    #include <direct.h>
#else
    #include <sys/stat.h>
#endif

#include "db.h"
#include "sqlite3.h"

#define SMOKE_DB "build/db_smoke.db"
#define SMOKE_DIR "build"

/* sqlite3_open 不会创建父目录，缺失时直接失败，所以这里先确保目录存在 */
static void ensure_dir(const char *dir)
{
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
}

static int g_pass = 0;
static int g_fail = 0;

static void check(const char *name, int ok)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
    }
}

static void check_int(const char *name, int got, int want)
{
    if (got == want) {
        check(name, 1);
    } else {
        printf("  [FAIL] %s (got %d, want %d)\n", name, got, want);
        g_fail++;
    }
}

/* ---- 用独立连接做旁路校验（只读/写少量元数据） ---- */

static int aux_exec(const char *sql)
{
    sqlite3 *db = NULL;
    char *err = NULL;
    int rc;

    if (sqlite3_open_v2(SMOKE_DB, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        if (db != NULL) {
            sqlite3_close(db);
        }
        return -1;
    }
    rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (err != NULL) {
        sqlite3_free(err);
    }
    sqlite3_close(db);
    return (rc == SQLITE_OK) ? 0 : -1;
}

static long long aux_scalar(const char *sql)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    long long v = -1;

    if (sqlite3_open_v2(SMOKE_DB, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        if (db != NULL) {
            sqlite3_close(db);
        }
        return -1;
    }
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            v = sqlite3_column_int64(st, 0);
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return v;
}

int main(void)
{
    user_t a, b, q;
    session_t s;
    char hash[64];
    char salt[64];
    char long_hash[501];
    char big[600];
    char small[8];

    /* 每次运行都从干净数据库开始 */
    ensure_dir(SMOKE_DIR);
    (void)remove(SMOKE_DB);
    (void)remove(SMOKE_DB "-wal");
    (void)remove(SMOKE_DB "-shm");

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    memset(&q, 0, sizeof(q));
    memset(&s, 0, sizeof(s));
    memset(long_hash, 'h', sizeof(long_hash) - 1);
    long_hash[sizeof(long_hash) - 1] = '\0';

    printf("== 1. 连接与建表 ==\n");
    check_int("未打开时 db_init_schema 返回 CHAT_ERR", db_init_schema(), CHAT_ERR);
    check_int("db_open", db_open(SMOKE_DB), CHAT_OK);
    check_int("db_open 重复调用安全", db_open(SMOKE_DB), CHAT_OK);
    check_int("db_init_schema #1", db_init_schema(), CHAT_OK);
    check_int("db_init_schema #2（幂等）", db_init_schema(), CHAT_OK);
    check_int("db_close 前查询正常", db_user_by_id(1, &q), CHAT_ERR_NOTFOUND);

    printf("== 2/3/4. 创建用户与唯一约束 ==\n");
    check_int("创建用户 A",
              db_user_create("alice", "hashA", "saltA", "10000001", "user_0001", &a), CHAT_OK);
    check("A.id > 0", a.id > 0);
    check_int("A.is_guest == 0", a.is_guest, 0);
    check("A.username == alice", strcmp(a.username, "alice") == 0);
    check("A.uid == 10000001", strcmp(a.uid, "10000001") == 0);
    check("A.handle == user_0001", strcmp(a.handle, "user_0001") == 0);
    check_int("重名用户 -> CHAT_ERR_TAKEN",
              db_user_create("alice", "h", "s", "10000002", "user_0002", NULL), CHAT_ERR_TAKEN);
    check_int("同 uid 不同名 -> CHAT_ERR_TAKEN",
              db_user_create("alice2", "h", "s", "10000001", "user_0003", NULL), CHAT_ERR_TAKEN);
    check_int("同 handle 不同名 -> CHAT_ERR_TAKEN",
              db_user_create("alice3", "h", "s", "10000005", "user_0001", NULL), CHAT_ERR_TAKEN);

    printf("== 5. 按用户名查 + 哈希/盐回传 ==\n");
    memset(hash, 0, sizeof(hash));
    memset(salt, 0, sizeof(salt));
    check_int("by_username(alice)",
              db_user_by_username("alice", &q, hash, sizeof(hash), salt, sizeof(salt)), CHAT_OK);
    check("回传 q.id 一致", q.id == a.id);
    check("回传 q.uid 一致", strcmp(q.uid, "10000001") == 0);
    check("回传 pass_hash 正确", strcmp(hash, "hashA") == 0);
    check("回传 salt 正确", strcmp(salt, "saltA") == 0);
    check_int("by_username 不存在 -> NOTFOUND",
              db_user_by_username("nobody", &q, NULL, 0, NULL, 0), CHAT_ERR_NOTFOUND);
    check_int("by_username 不需要 hash/salt（NULL）",
              db_user_by_username("alice", &q, NULL, 0, NULL, 0), CHAT_OK);

    printf("== 6. 按 uid/handle/id 查 ==\n");
    check_int("by_uid", db_user_by_uid("10000001", &q), CHAT_OK);
    check("by_uid 命中 A", q.id == a.id);
    check_int("by_uid 不存在", db_user_by_uid("99999999", &q), CHAT_ERR_NOTFOUND);
    check_int("by_handle", db_user_by_handle("user_0001", &q), CHAT_OK);
    check("by_handle 命中 A", q.id == a.id);
    check_int("by_handle 不存在", db_user_by_handle("nope", &q), CHAT_ERR_NOTFOUND);
    check_int("by_id", db_user_by_id(a.id, &q), CHAT_OK);
    check("by_id 命中 A", strcmp(q.username, "alice") == 0);
    check_int("by_id 不存在", db_user_by_id(999999, &q), CHAT_ERR_NOTFOUND);

    printf("== 7. 修改身份码 ==\n");
    check_int("set_handle(A,newcode)", db_user_set_handle(a.id, "newcode"), CHAT_OK);
    check_int("by_handle(newcode)", db_user_by_handle("newcode", &q), CHAT_OK);
    check("newcode 属于 A", q.id == a.id);
    check_int("旧 handle 已不存在", db_user_by_handle("user_0001", &q), CHAT_ERR_NOTFOUND);
    check_int("handle_taken(newcode,-1) == 1", db_user_handle_taken("newcode", -1), 1);
    check_int("handle_taken(newcode,A.id) == 0", db_user_handle_taken("newcode", a.id), 0);
    check_int("handle_taken(未用) == 0", db_user_handle_taken("free_code", -1), 0);
    check_int("set_handle(不存在 id) -> NOTFOUND",
              db_user_set_handle(999999, "whatever"), CHAT_ERR_NOTFOUND);

    printf("== 8. 用户 B 抢占用中的身份码 ==\n");
    check_int("创建用户 B",
              db_user_create("bob", "hashB", "saltB", "10000002", "user_0002", &b), CHAT_OK);
    check_int("B 改成 A 的 handle -> CHAT_ERR_TAKEN",
              db_user_set_handle(b.id, "newcode"), CHAT_ERR_TAKEN);
    check_int("B 的原 handle 未变", db_user_by_handle("user_0002", &q), CHAT_OK);
    check("user_0002 仍是 B", q.id == b.id);

    printf("== 9. 会话增查改删 ==\n");
    check_int("db_session_create(deadbeef)",
              db_session_create("deadbeefdeadbeef", a.id, 0, "10000001", "newcode", "alice"), CHAT_OK);
    check_int("db_session_get",
              db_session_get("deadbeefdeadbeef", &s), CHAT_OK);
    check("会话 token 回填", strcmp(s.token, "deadbeefdeadbeef") == 0);
    check("会话 user_id", s.user_id == a.id);
    check_int("会话 is_guest == 0", s.is_guest, 0);
    check("会话 uid", strcmp(s.uid, "10000001") == 0);
    check("会话 handle", strcmp(s.handle, "newcode") == 0);
    check("会话 username", strcmp(s.username, "alice") == 0);
    check_int("不存在的 token -> NOTFOUND",
              db_session_get("nosuchtoken", &s), CHAT_ERR_NOTFOUND);

    check_int("把 last_seen 置 1", aux_exec("UPDATE sessions SET last_seen = 1 WHERE token = 'deadbeefdeadbeef'"), 0);
    db_session_touch("deadbeefdeadbeef");
    check("db_session_touch 更新时间戳",
          aux_scalar("SELECT last_seen FROM sessions WHERE token = 'deadbeefdeadbeef'") > 1);
    check_int("touch 后会话仍在", db_session_get("deadbeefdeadbeef", &s), CHAT_OK);

    db_session_delete("deadbeefdeadbeef");
    check_int("db_session_delete 生效",
              db_session_get("deadbeefdeadbeef", &s), CHAT_ERR_NOTFOUND);
    db_session_delete("nosuchtoken");   /* 删除不存在的 token 不应崩溃 */
    check("删除不存在的 token 后连接仍可用", db_session_get("x", &s) == CHAT_ERR_NOTFOUND);

    printf("== 10. 游客会话清理 ==\n");
    check_int("创建游客会话",
              db_session_create("guesttoken0001", 0, 1, "30000001", "", "guest"), CHAT_OK);
    check_int("创建普通会话",
              db_session_create("normaltoken0001", a.id, 0, "10000001", "newcode", "alice"), CHAT_OK);
    db_session_clear_guests();
    check_int("游客会话已被清理",
              db_session_get("guesttoken0001", &s), CHAT_ERR_NOTFOUND);
    check_int("普通会话保留",
              db_session_get("normaltoken0001", &s), CHAT_OK);
    db_session_clear_guests();          /* 重复清理安全 */
    check_int("再次清理后普通会话仍在",
              db_session_get("normaltoken0001", &s), CHAT_OK);

    printf("== 11. 哈希/盐缓冲不足 -> CHAT_ERR_FULL ==\n");
    check_int("创建超长哈希用户",
              db_user_create("carol", long_hash, "saltC", "10000003", "user_0003", NULL), CHAT_OK);
    check_int("小缓冲 hash -> CHAT_ERR_FULL",
              db_user_by_username("carol", &q, small, sizeof(small), salt, sizeof(salt)),
              CHAT_ERR_FULL);
    check_int("小缓冲 salt -> CHAT_ERR_FULL",
              db_user_by_username("carol", &q, hash, sizeof(hash), small, sizeof(small)),
              CHAT_ERR_FULL);
    check_int("hash_n == 0 -> CHAT_ERR_FULL",
              db_user_by_username("carol", &q, hash, 0, NULL, 0), CHAT_ERR_FULL);
    check_int("足够大的缓冲可取回 500 字节哈希",
              db_user_by_username("carol", &q, big, sizeof(big), salt, sizeof(salt)), CHAT_OK);
    check("取回的超长哈希内容完整", strcmp(big, long_hash) == 0);

    printf("== 12. 关闭 ==\n");
    db_close();
    db_close();                          /* 可重复调用 */
    check_int("关闭后 db_init_schema -> CHAT_ERR", db_init_schema(), CHAT_ERR);
    check_int("关闭后 by_uid -> NOTFOUND", db_user_by_uid("10000001", &q), CHAT_ERR_NOTFOUND);
    check_int("关闭后 handle_taken == 0", db_user_handle_taken("newcode", -1), 0);

    /* 清理临时数据库，避免留下无用文件 */
    (void)remove(SMOKE_DB);
    (void)remove(SMOKE_DB "-wal");
    (void)remove(SMOKE_DB "-shm");

    printf("\n===== 汇总: %d PASS / %d FAIL =====\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
