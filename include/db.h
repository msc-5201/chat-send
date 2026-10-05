/*
 * db.h —— SQLite 数据层（契约文件，冻结后只读）
 *
 * 所有函数线程安全：内部使用全局互斥锁保护同一个连接。
 * 返回 CHAT_OK 表示成功，CHAT_ERR_* 表示失败。
 */
#ifndef DB_H
#define DB_H

#include <sqlite3.h>

#include "chat.h"

/* ================= db.c：连接与建表 ================= */

int  db_open(const char *path);
void db_close(void);
/* 创建全部表（幂等）。成功返回 CHAT_OK */
int  db_init_schema(void);

/* ---- 供 db_user.c / db_session.c / db_friend.c / db_message.c 内部共用 ----
 * 服务器是「每连接一线程」，所有数据访问都必须在 db_lock()/db_unlock() 保护下进行。
 *
 * 推荐用法：
 *     db_lock();
 *     sqlite3 *h = db_handle();
 *     ... 使用 h 做 prepare/bind/step/finalize ...
 *     db_unlock();
 *
 * 注意：
 *   - db_lock/db_unlock 不可重入，锁内不要再次调用会自己加锁的 db_* 函数。
 *   - db_handle() 只在 db_open() 成功后才返回非 NULL。
 */
sqlite3 *db_handle(void);
void db_lock(void);
void db_unlock(void);

/* ================= db_user.c：用户表 ================= */

/* 创建用户。uid / handle 由调用方生成。成功时填写 out_user（含 id）。
 * 用户名 / uid / handle 冲突返回 CHAT_ERR_TAKEN */
int db_user_create(const char *username, const char *pass_hash, const char *salt,
                   const char *uid, const char *handle, user_t *out_user);

/* 按用户名查用户，并取回密码哈希与盐。
 * 找不到返回 CHAT_ERR_NOTFOUND；hash/salt 可以为 NULL 表示不需要 */
int db_user_by_username(const char *username, user_t *out,
                        char *hash, size_t hash_n, char *salt, size_t salt_n);

/* 按标识码查用户 */
int db_user_by_uid(const char *uid, user_t *out);

/* 按身份码查用户 */
int db_user_by_handle(const char *handle, user_t *out);

/* 按 id 查用户 */
int db_user_by_id(int id, user_t *out);

/* 修改身份码。新身份码被占用返回 CHAT_ERR_TAKEN */
int db_user_set_handle(int id, const char *handle);

/* 身份码是否已被别人占用（exclude_id 为 -1 表示不排除任何人） */
int db_user_handle_taken(const char *handle, int exclude_id);

/* 修改密码：整体替换 pass_hash 与 salt（调用方须已用新盐算好哈希）。
 * 成功返回 CHAT_OK；用户不存在返回 CHAT_ERR_NOTFOUND */
int db_user_set_password(int id, const char *pass_hash, const char *salt);

/* 仪表盘用的个人统计（一次加锁内完成三条查询）：
 * 注册时间（Unix 秒）、好友数、发言数（含世界与私聊）。
 * 用户不存在返回 CHAT_ERR_NOTFOUND；三个出参均不可为 NULL。 */
int db_user_stats(int user_id, long *created_at, int *friend_count, int *message_count);

/* ================= db_session.c：会话表 ================= */

/* 创建会话，token 由调用方生成 */
int db_session_create(const char *token, int user_id, int is_guest,
                      const char *uid, const char *handle, const char *username);

/* 按 token 取会话，找不到返回 CHAT_ERR_NOTFOUND */
int db_session_get(const char *token, session_t *out);

void db_session_touch(const char *token);
void db_session_delete(const char *token);
/* 服务器启动时清理遗留的游客会话 */
void db_session_clear_guests(void);

/* 修改密码后踢掉该用户的其它登录会话（保留 keep_token 这一个）。
 * 返回被删除的会话数。 */
int db_session_delete_others(int user_id, const char *keep_token);

/* ================= db_friend.c：好友 ================= */

/* 发送好友申请。已申请/已是好友返回 CHAT_ERR_CONFLICT，加自己返回 CHAT_ERR */
int db_friend_add_request(int from_id, int to_id);

/* 待处理申请列表，写成 JSON 数组片段（"pending":[...] 的值部分），
 * 每项形如 {"id":1,"from_id":2,"username":"tom","handle":"user_1234"} */
int db_friend_requests_json(int user_id, char *out, size_t n);

/* 处理申请：accept 非 0 表示同意（双向写入 friends 表），否则拒绝。
 * 只有收件人可以处理，否则返回 CHAT_ERR */
int db_friend_set_request(int req_id, int user_id, int accept);

/* 好友列表，写成 JSON 数组片段，每项形如
 * {"id":2,"username":"tom","uid":"48210937","handle":"user_1234","is_guest":0} */
int db_friend_list_json(int user_id, char *out, size_t n);

/* 两人是否为好友，是返回 CHAT_OK，否则 CHAT_ERR_NOTFOUND */
int db_friend_are(int a, int b);

/* 好友关系版本号：任何好友申请 / 处理成功后 +1。
 * /api/poll 把它带给前端，前端发现变化即刷新好友列表，
 * 这样「乙同意申请后，甲不用刷新页面就能看到乙」。 */
long db_friend_rev(void);

/* ================= db_message.c：消息 ================= */

/* 追加一条消息，返回新消息 id，失败返回 CHAT_ERR */
int db_msg_add(const char *room, int sender_id, const char *sender, const char *body);

/* 取 room 中 id > after_id 的消息，写成 JSON 数组片段，每项形如
 * {"id":12,"sender_id":2,"sender":"tom","body":"hi","ts":"2026-10-04 16:00:00"} */
int db_msg_fetch_json(const char *room, long after_id, char *out, size_t n);

#endif /* DB_H */
