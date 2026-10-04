/*
 * http.h —— HTTP 解析、响应与静态文件（契约文件，冻结后只读）
 */
#ifndef HTTP_H
#define HTTP_H

#include "chat.h"

/* 从套接字读取并解析一个请求。成功返回 CHAT_OK */
int http_read_request(int fd, request_t *req);

/* 发送响应。body 为已构造好的 JSON 字符串 */
void http_respond_json(int fd, int status, const char *json);

/* 同上，但附带 Set-Cookie: sid=<token>；token 为 NULL 时不发送 Cookie */
void http_respond_json_cookie(int fd, int status, const char *json, const char *token);

/* 发送任意文本响应 */
void http_respond_text(int fd, int status, const char *content_type, const char *body);

/* 直接把套接字当作文件描述符发送（HTTP 头已由本函数构造） */
void http_respond_raw(int fd, int status, const char *content_type, const char *data, size_t len);

/* 托管 CHAT_STATIC_ROOT 下的静态文件。
 * 成功返回 CHAT_OK；找不到返回 CHAT_ERR_NOTFOUND；路径非法或读取失败返回 CHAT_ERR */
int http_serve_static(int fd, const char *path);

/* 从表单请求体中取参数（application/x-www-form-urlencoded）。取到返回 CHAT_OK */
int http_param(const request_t *req, const char *key, char *out, size_t n);

/* 从查询串中取参数。取到返回 CHAT_OK */
int http_query_param(const request_t *req, const char *key, char *out, size_t n);

/* 从 Cookie 中取 jsession 值。取到返回 CHAT_OK */
int http_cookie_sid(const request_t *req, char *out, size_t n);

/* 状态码对应的英文短语，例如 200 -> "OK" */
const char *http_status_text(int status);

#endif /* HTTP_H */
