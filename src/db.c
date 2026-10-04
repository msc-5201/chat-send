/*
 * db.c —— SQLite 连接、建表与全局互斥锁
 *
 * 连接句柄与锁均为本文件的 static 全局（不对外暴露内部头文件），仅通过
 * include/db.h 中冻结的 db_handle() / db_lock() / db_unlock() 供
 * db_user.c / db_session.c（以及 P3b 的 db_friend.c / db_message.c）共用。
 *
 * 锁约定（与 db.h 注释一致）：
 *   - db_lock/db_unlock 不可重入：锁内不得再调用会自己加锁的 db_* 公开函数。
 *   - 未 db_open() 时调用 db_lock/db_unlock 也安全，不会崩溃。
 */
#include <stdio.h>

#include "db.h"

#ifdef _WIN32
#include <windows.h>
static CRITICAL_SECTION g_lock;
/* 0=未初始化 1=初始化中 2=就绪 3=销毁中（仅 Windows 分支使用） */
static volatile LONG    g_lock_state = 0;
#else
#include <pthread.h>
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

static sqlite3 *g_db = NULL;

/* ---- 锁 ---- */

#ifdef _WIN32
/* 懒初始化临界区，保证 db_lock 在未 db_open() 时也可用 */
static void lock_ensure(void)
{
    for (;;) {
        LONG st = InterlockedCompareExchange(&g_lock_state, 1, 0);
        if (st == 0) {                       /* 由本线程完成初始化 */
            InitializeCriticalSection(&g_lock);
            InterlockedExchange(&g_lock_state, 2);
            return;
        }
        if (st == 2) {                       /* 已就绪 */
            return;
        }
        Sleep(0);                            /* 别的线程正在初始化，稍后重试 */
    }
}
#endif

void db_lock(void)
{
#ifdef _WIN32
    lock_ensure();
    EnterCriticalSection(&g_lock);
#else
    pthread_mutex_lock(&g_lock);
#endif
}

void db_unlock(void)
{
#ifdef _WIN32
    /* 必须与 db_lock() 严格配对：不能加条件判断。
     * 一旦某线程 Enter 之后条件不成立而跳过 Leave，这把锁就永久不会释放。 */
    LeaveCriticalSection(&g_lock);
#else
    pthread_mutex_unlock(&g_lock);
#endif
}

sqlite3 *db_handle(void)
{
    return g_db;
}

/* ---- 建表 SQL（幂等；字段名与其他包依赖的契约一致，勿改） ---- */

static const char *const k_schema_sql =
    "PRAGMA journal_mode=WAL;"
    "PRAGMA foreign_keys=ON;"
    "CREATE TABLE IF NOT EXISTS users ("
    "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  username   TEXT NOT NULL UNIQUE,"
    "  pass_hash  TEXT NOT NULL,"
    "  salt       TEXT NOT NULL,"
    "  uid        TEXT NOT NULL UNIQUE,"
    "  handle     TEXT NOT NULL UNIQUE,"
    "  created_at INTEGER NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS sessions ("
    "  token      TEXT PRIMARY KEY,"
    "  user_id    INTEGER NOT NULL,"
    "  is_guest   INTEGER NOT NULL,"
    "  uid        TEXT NOT NULL,"
    "  handle     TEXT NOT NULL DEFAULT '',"
    "  username   TEXT NOT NULL,"
    "  created_at INTEGER NOT NULL,"
    "  last_seen  INTEGER NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS friend_requests ("
    "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  from_user  INTEGER NOT NULL,"
    "  to_user    INTEGER NOT NULL,"
    "  status     TEXT NOT NULL DEFAULT 'pending',"
    "  created_at INTEGER NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS friends ("
    "  user_id    INTEGER NOT NULL,"
    "  friend_id  INTEGER NOT NULL,"
    "  created_at INTEGER NOT NULL,"
    "  PRIMARY KEY (user_id, friend_id)"
    ");"
    "CREATE TABLE IF NOT EXISTS messages ("
    "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  room       TEXT NOT NULL,"
    "  sender_id  INTEGER NOT NULL,"
    "  sender     TEXT NOT NULL,"
    "  body       TEXT NOT NULL,"
    "  created_at INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_messages_room_id ON messages(room, id);";

/* ---- 连接生命周期 ---- */

int db_open(const char *path)
{
    sqlite3 *db = NULL;
    int rc;

    if (path == NULL) {
        return CHAT_ERR;
    }

    db_lock();
    if (g_db != NULL) {              /* 已打开：重复调用安全，直接成功返回 */
        db_unlock();
        return CHAT_OK;
    }
    rc = sqlite3_open_v2(path, &db,
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                         NULL);
    if (rc != SQLITE_OK) {
        if (db != NULL) {
            sqlite3_close(db);
        }
        db_unlock();
        return CHAT_ERR;
    }
    sqlite3_busy_timeout(db, 5000);
    sqlite3_exec(db, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);
    g_db = db;
    db_unlock();
    return CHAT_OK;
}

void db_close(void)
{
    db_lock();
    if (g_db != NULL) {
        sqlite3_close(g_db);
        g_db = NULL;
    }
    db_unlock();

    /* 刻意不调用 DeleteCriticalSection：
     * 服务器是「每连接一线程」，退出路径上可能仍有被 detach 的工作线程在跑，
     * 销毁一把可能正被持有（或即将被获取）的临界区是未定义行为。
     * 这个锁对象只在进程内创建一次，交给操作系统在进程退出时回收即可。 */
}

int db_init_schema(void)
{
    sqlite3 *h;
    char *err = NULL;
    int rc;

    db_lock();
    h = db_handle();
    if (h == NULL) {
        db_unlock();
        return CHAT_ERR;
    }
    rc = sqlite3_exec(h, k_schema_sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        if (err != NULL) {
            fprintf(stderr, "[db] init schema failed: %s\n", err);
            sqlite3_free(err);
        }
        db_unlock();
        return CHAT_ERR;
    }
    db_unlock();
    return CHAT_OK;
}
