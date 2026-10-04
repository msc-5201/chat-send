/*
 * message.h —— 消息路由（契约文件，冻结后只读）
 */
#ifndef MESSAGE_H
#define MESSAGE_H

#include "chat.h"

int message_fetch(const request_t *req, int fd); /* GET  /api/messages */
int message_send(const request_t *req, int fd);  /* POST /api/messages */
int message_poll(const request_t *req, int fd);  /* GET  /api/poll     */

#endif /* MESSAGE_H */
