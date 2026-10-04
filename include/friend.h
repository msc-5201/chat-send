/*
 * friend.h —— 好友路由（契约文件，冻结后只读）
 */
#ifndef FRIEND_H
#define FRIEND_H

#include "chat.h"

int friend_list(const request_t *req, int fd);   /* GET  /api/friends        */
int friend_add(const request_t *req, int fd);    /* POST /api/friends/add    */
int friend_accept(const request_t *req, int fd); /* POST /api/friends/accept */
int friend_reject(const request_t *req, int fd); /* POST /api/friends/reject */

#endif /* FRIEND_H */
