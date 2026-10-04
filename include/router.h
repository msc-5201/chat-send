/*
 * router.h —— 路由分发（契约文件，冻结后只读）
 */
#ifndef ROUTER_H
#define ROUTER_H

#include "chat.h"

/* 根据 req->method / req->path 找到处理函数并执行，未匹配时返回 404 */
void router_dispatch(int fd, const request_t *req);

#endif /* ROUTER_H */
