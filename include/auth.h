/*
 * auth.h —— 鉴权与账号路由（契约文件，冻结后只读）
 *
 * 每个路由处理函数负责：读取参数 -> 完成业务 -> 自己调用 http_respond_* 发送响应，
 * 最后返回 CHAT_OK（返回值仅用于日志）。
 */
#ifndef AUTH_H
#define AUTH_H

#include "chat.h"

int auth_register(const request_t *req, int fd);   /* POST /api/register */
int auth_login(const request_t *req, int fd);      /* POST /api/login    */
int auth_guest(const request_t *req, int fd);      /* POST /api/guest    */
int auth_logout(const request_t *req, int fd);     /* POST /api/logout   */
int auth_me(const request_t *req, int fd);         /* GET  /api/me       */
int auth_set_handle(const request_t *req, int fd); /* POST /api/handle   */
int auth_set_password(const request_t *req, int fd); /* POST /api/password */

/* 从请求的 Cookie 中解析当前会话。成功返回 CHAT_OK 并填写 out。
 * 供 friend.c / message.c 复用。 */
int auth_from_request(const request_t *req, session_t *out);

#endif /* AUTH_H */
