/*
 * dashboard.h —— 用户仪表盘（契约文件）
 *
 * 每个注册用户在 <CHAT_DASHBOARD_ROOT>/<用户名>/<用户名>.json 下有一份个人数据文件，
 * 形如 Dashboard/alice/alice.json（根目录见 include/chat.h 的 CHAT_DASHBOARD_ROOT）。
 *
 * 文件只含非敏感数据，**绝不**写入密码哈希、盐或会话 token。
 */
#ifndef DASHBOARD_H
#define DASHBOARD_H

#include "chat.h"

/* 把用户个人数据写入 / 刷新到 Dashboard/<用户名>/<用户名>.json（目录不存在则创建）。
 * 注册成功时调用一次；GET /api/dashboard 时也会调用，用于自愈（文件被删后自动重建）。
 * 返回 CHAT_OK 表示文件已写好；其它值表示失败——调用方不应因此中断主流程。 */
int dashboard_write_user(const user_t *u);

int dashboard_get(const request_t *req, int fd);   /* GET /api/dashboard */

#endif /* DASHBOARD_H */
