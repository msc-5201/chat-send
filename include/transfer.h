/*
 * transfer.h —— 点对点文件传输信令路由（契约文件，冻结后只读）
 *
 * 服务器只负责在好友之间转发 WebRTC 信令（offer / answer / ice），
 * 文件字节不经过服务器。信令信箱是内存态，不持久化。
 */
#ifndef TRANSFER_H
#define TRANSFER_H

#include "chat.h"

int transfer_send(const request_t *req, int fd); /* POST /api/transfer/send */
int transfer_poll(const request_t *req, int fd); /* GET  /api/transfer/poll */

#endif /* TRANSFER_H */
